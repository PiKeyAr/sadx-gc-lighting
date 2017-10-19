struct VS_IN
{
	float3 position : POSITION;
	float3 normal   : NORMAL;
	float2 tex      : TEXCOORD0;
	float4 color    : COLOR0;
};

struct PS_IN
{
	float4 position    : POSITION0;
	float4 diffuse     : COLOR0;
	float2 tex         : TEXCOORD0;
	float3 worldNormal : TEXCOORD1;
	float3 halfVector  : TEXCOORD2;
	float2 depth       : TEXCOORD3;
	float  fogDist     : FOG;
};

struct StageLight
{
	float3 direction;
	float specular;
	float multiplier;
	float3 diffuse;
	float3 ambient;
	float padding[5];
};

// From FixedFuncEMU.fx
// Copyright (c) 2005 Microsoft Corporation. All rights reserved.
#define FOGMODE_NONE   0
#define FOGMODE_EXP    1
#define FOGMODE_EXP2   2
#define FOGMODE_LINEAR 3
#define E 2.71828

#define D3DMCS_MATERIAL 0 // Color from material is used
#define D3DMCS_COLOR1   1 // Diffuse vertex color is used
#define D3DMCS_COLOR2   2 // Specular vertex color is used

// This never changes
static const float AlphaRef = 16.0f / 255.0f;

// Diffuse texture
Texture2D BaseTexture : register(t0);
Texture2D DepthTexture : register(t1);

float4x4 WorldMatrix : register(c0);
float4x4 wvMatrix : register(c4);
float4x4 ProjectionMatrix : register(c8);
// The inverse transpose of the world view matrix - used for environment mapping.
float4x4 wvMatrixInvT : register(c12);

// Used primarily for environment mapping.
float4x4 TextureTransform : register(c16) = {
	-0.5, 0.0, 0.0, 0.0,
	0.0, 0.5, 0.0, 0.0,
	0.0, 0.0, 1.0, 0.0,
	0.5, 0.5, 0.0, 1.0
};

uint DiffuseSource : register(c20) = (uint)D3DMCS_COLOR1;
float4 MaterialDiffuse : register(c21) = float4(1.0f, 1.0f, 1.0f, 1.0f);

float3 LightDirection : register(c26) = float3(0.0f, -1.0f, 0.0f);
float3 NormalScale : register(c27) = float3(1, 1, 1);

uint   FogMode : register(c28) = (uint)FOGMODE_NONE;
float  FogStart : register(c29);
float  FogEnd : register(c30);
float  FogDensity : register(c31);
float4 FogColor : register(c32);
float3 CameraPosition : register(c33);

float4 MaterialSpecular : register(c39) = float4(0.0f, 0.0f, 0.0f, 0.0f);
float  MaterialPower : register(c40) = 1.0f;

float4 LightDiffuse : register(c41);
float4 LightSpecular : register(c42);
float4 LightAmbient : register(c43);

float ParticleScale : register(c44);
float DepthOverride : register(c45);
float DrawDistance : register(c46);
float2 ViewPort : register(c47);

// Samplers
SamplerState baseSampler : register(s0) = sampler_state
{
	Texture = BaseTexture;
};

#define DEFAULT_SAMPLER \
	MinFilter = Point;\
	MagFilter = Point;\
	AddressU  = Clamp;\
	AddressV  = Clamp;\
	ColorOp   = Modulate;\
	ColorArg1 = Texture;\
	ColorArg2 = Current;\
	AlphaOp   = SelectArg1;\
	AlphaArg1 = Texture;\
	AlphaArg2 = Current

SamplerState depthSampler : register(s1) = sampler_state
{
	Texture = DepthTexture;
	DEFAULT_SAMPLER;
};

// Helpers

#ifdef USE_FOG
// From FixedFuncEMU.fx
// Copyright (c) 2005 Microsoft Corporation. All rights reserved.

float CalcFogFactor(float d)
{
	float fogCoeff;

	switch (FogMode)
	{
		default:
			break;

		case FOGMODE_EXP:
			fogCoeff = 1.0 / pow(E, d * FogDensity);
			break;

		case FOGMODE_EXP2:
			fogCoeff = 1.0 / pow(E, d * d * FogDensity * FogDensity);
			break;

		case FOGMODE_LINEAR:
			fogCoeff = (FogEnd - d) / (FogEnd - FogStart);
			break;
	}

	return clamp(fogCoeff, 0, 1);
}
#endif

float4 GetDiffuse(in float4 vcolor)
{
	float4 color = (DiffuseSource == D3DMCS_COLOR1 && any(vcolor)) ? vcolor : MaterialDiffuse;

#if 0
	int3 icolor = color.rgb * 255.0;
	if (icolor.r == 178 && icolor.g == 178 && icolor.b == 178)
	{
		return float4(1, 1, 1, color.a);
	}
#endif

	return color;
}

// Vertex shaders

PS_IN vs_main(VS_IN input)
{
	PS_IN output;

	output.position = mul(float4(input.position, 1), wvMatrix);
	output.fogDist = output.position.z;
	output.position = mul(output.position, ProjectionMatrix);
	output.depth = output.position.zw;

#if defined(USE_TEXTURE) && defined(USE_ENVMAP)
	output.tex = (float2)mul(float4(input.normal, 1), wvMatrixInvT);
	output.tex = (float2)mul(float4(output.tex, 0, 1), TextureTransform);
#else
	output.tex = input.tex;
#endif

	output.diffuse = GetDiffuse(input.color);
	output.worldNormal = mul(input.normal * NormalScale, (float3x3)WorldMatrix);

	float3 worldPos = mul(float4(input.position, 1), WorldMatrix).xyz;
	output.halfVector = normalize(normalize(CameraPosition - worldPos) + normalize(LightDirection));

	return output;
}

float4 ps_main(PS_IN input, in float2 vpos : VPOS) : COLOR
{
	float4 result;

	float4 ambient = 0;
	float4 diffuse = 0;
	float4 specular = 0;

#ifdef USE_LIGHT
	ambient = float4(LightAmbient.rgb, 0);

	float d = dot(normalize(LightDirection), input.worldNormal);

	float3 combined = saturate(LightDiffuse.rgb * d);
	diffuse = float4(combined, 1);

	// Apply the ambient, clamping it to a sane value.
	diffuse = saturate(diffuse + ambient);

	// Input diffuse is specifically applied after
	// everything else to ensure its vibrancy.
	diffuse *= input.diffuse;

	float3 normal = input.worldNormal;

#ifdef USE_SMOOTH_LIGHTING
	normal = normalize(normal);
#endif

	// funny joke
	float d2 = dot(normal, input.halfVector);

	// TODO: fix material power of 0 (for real though)
	specular.rgb = MaterialSpecular.rgb * saturate(LightSpecular.rgb * pow(max(0.0001f, d2), max(1.0, MaterialPower)));
#elif defined(SOFT_PARTICLE)
	diffuse = float4(1, 1, 1, 1);
#else
	diffuse = input.diffuse;
#endif

#ifdef USE_TEXTURE
	result = tex2D(baseSampler, input.tex);
	result = (result * diffuse) + specular;
#else
	result = diffuse + specular;
#endif

#ifdef USE_ALPHA
	clip(result.a < AlphaRef ? -1 : 1);
#endif

#if defined(DEPTH_MAP)
	//return float4(input.depth.x / input.depth.y, 0, 0, 1);
	return float4(input.fogDist, 0, 0, 1);
#elif defined(SOFT_PARTICLE)

	float2 coord = vpos / (ViewPort - 0.5);

	//float particleDepth = input.depth.x / input.depth.y;
	float particleDepth = input.fogDist;
	float depthSample = tex2D(depthSampler, coord).r;

#if 0
	float4 depthViewSample = mul(-ProjectionMatrix, float4(coord, depthSample, 1));
	float4 depthViewParticle = mul(-ProjectionMatrix, float4(coord, particleDepth, 1));

	float depthDiff = (depthViewSample.z / depthViewSample.w) - (depthViewParticle.z / depthViewParticle.w);
#else
	float depthDiff = (depthSample - particleDepth) / ParticleScale;
#endif
	
	if (depthDiff < 0)
	{
		discard;
	}

	float depthFade = saturate(depthDiff /* / g_fFadeDistance */);
	result.a *= depthFade;

#else
	// debug depth output
	//float f = (input.fogDist / DrawDistance);
	//return float4(f, f, f, 1);
#endif

#ifdef USE_FOG
	float factor = CalcFogFactor(input.fogDist);
	result.rgb = (factor * result + (1.0 - factor) * FogColor).rgb;
#endif

	return result;
}

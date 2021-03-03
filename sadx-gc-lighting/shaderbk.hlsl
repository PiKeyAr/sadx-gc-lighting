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
	float  fogDist     : FOG;
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

// Diffuse sampler
SamplerState baseSampler : register(s0) = sampler_state
{
	Texture = BaseTexture;
};

// Parameters

float4x4 WorldMatrix      : register(c0);
float4x4 wvMatrix         : register(c4);
float4x4 ProjectionMatrix : register(c8);
float4x4 wvMatrixInvT     : register(c12); // Inverse transpose world view - used for environment mapping.

// Used primarily for environment mapping.
float4x4 TextureTransform : register(c16) = {
	-0.5, 0.0, 0.0, 0.0,
	0.0, 0.5, 0.0, 0.0,
	0.0, 0.0, 1.0, 0.0,
	0.5, 0.5, 0.0, 1.0
};

float3 NormalScale     : register(c20) = float3(1, 1, 1);
float3 LightDirection  : register(c21) = float3(0.0f, -1.0f, 0.0f);
uint   DiffuseSource   : register(c22) = (uint)D3DMCS_COLOR1;
float4 MaterialDiffuse : register(c23) = float4(1.0f, 1.0f, 1.0f, 1.0f);

// FogMode cannot be merged with FogConfig because of
// Shader Model 3 restrictions on acceptable values.
uint FogMode : register(c24) = (uint)FOGMODE_NONE;
// x y and z are start, end, and density respectively
float3 FogConfig : register(c25);
float4 FogColor  : register(c26);

float3 CameraPosition : register(c27);

float4 MaterialSpecular : register(c28) = float4(0.0f, 0.0f, 0.0f, 0.0f);
float  MaterialPower    : register(c29) = 1.0f;

float4 LightDiffuse  : register(c30);
float4 LightSpecular : register(c31);
float4 LightAmbient  : register(c32);

// Helpers

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
			fogCoeff = 1.0 / pow(E, d * FogConfig.z);
			break;

		case FOGMODE_EXP2:
			fogCoeff = 1.0 / pow(E, d * d * FogConfig.z * FogConfig.z);
			break;

		case FOGMODE_LINEAR:
			fogCoeff = (FogConfig.y - d) / (FogConfig.y - FogConfig.x);
			break;
	}

	return clamp(fogCoeff, 0, 1);
}

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

PS_IN vs_main(VS_IN input)
{
	PS_IN output;

	output.position = mul(float4(input.position, 1), wvMatrix);
	output.fogDist = output.position.z;
	output.position = mul(output.position, ProjectionMatrix);

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

float4 ps_main(PS_IN input) : COLOR
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

	#ifdef USE_SPECULAR
	{
		float3 normal = input.worldNormal;

	#ifdef USE_SMOOTH_LIGHTING
		normal = normalize(normal);
	#endif

		// funny joke
		float d2 = dot(normal, input.halfVector);

		// TODO: fix material power of 0 (for real though)
		specular.rgb = MaterialSpecular.rgb * saturate(LightSpecular.rgb * pow(max(0.0001f, d2), max(1.0, MaterialPower)));
	}
	#endif
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

#ifdef USE_FOG
	float factor = CalcFogFactor(input.fogDist);
	result.rgb = (factor * result + (1.0 - factor) * FogColor).rgb;
#endif

	return result;
}

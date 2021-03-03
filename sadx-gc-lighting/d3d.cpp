#include "stdafx.h"

#include <Windows.h>
#include <Wincrypt.h>

// Direct3D
#include <d3dx9.h>

// d3d8to9
#include <d3d8to9.hpp>

// Mod loader
#include <SADXModLoader.h>
#include <Trampoline.h>

// MinHook
#include <MinHook.h>

// Standard library
#include <iomanip>
#include <sstream>
#include <vector>
#include <unordered_map>

// Local
#include "d3d.h"
#include "datapointers.h"
#include "globals.h"
#include "ShaderParameter.h"
#include "FileSystem.h"

namespace param
{
	ShaderParameter<D3DXMATRIX>  WorldMatrix(0, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  wvMatrix(4, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  ProjectionMatrix(8, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  wvMatrixInvT(12, {}, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXMATRIX>  TextureTransform(16, {}, IShaderParameter::Type::vertex);

	ShaderParameter<D3DXVECTOR3> NormalScale(20, { 1.0f, 1.0f, 1.0f }, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXVECTOR3> LightDirection(21, { 0.0f, -1.0f, 0.0f }, IShaderParameter::Type::both);
	ShaderParameter<int>         DiffuseSource(22, 0, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXCOLOR>   MaterialDiffuse(23, {}, IShaderParameter::Type::vertex);

	ShaderParameter<int>         FogMode(24, 0, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXVECTOR3> FogConfig(25, {}, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXCOLOR>   FogColor(26, {}, IShaderParameter::Type::pixel);

	ShaderParameter<D3DXVECTOR3> CameraPosition(27, { 0.0f, 0.0f, 0.0f }, IShaderParameter::Type::vertex);
	ShaderParameter<D3DXCOLOR>   MaterialSpecular(28, {}, IShaderParameter::Type::pixel);
	ShaderParameter<float>       MaterialPower(29, 1.0f, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXCOLOR>   LightDiffuse(30, {}, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXCOLOR>   LightSpecular(31, {}, IShaderParameter::Type::pixel);
	ShaderParameter<D3DXCOLOR>   LightAmbient(32, {}, IShaderParameter::Type::pixel);
	
	IShaderParameter* const parameters[] = {
		&WorldMatrix,
		&wvMatrix,
		&ProjectionMatrix,
		&wvMatrixInvT,
		&TextureTransform,
		&NormalScale,
		&LightDirection,
		&DiffuseSource,
		&MaterialDiffuse,
		&FogMode,
		&FogConfig,
		&FogColor,
		&CameraPosition,
		&MaterialSpecular,
		&MaterialPower,
		&LightDiffuse,
		&LightSpecular,
		&LightAmbient
	};

	static void release_parameters()
	{
		for (auto& i : parameters)
		{
			i->release();
		}
	}
}

namespace local
{
	static Trampoline* Direct3D_PerformLighting_t         = nullptr;
	static Trampoline* sub_77EAD0_t                       = nullptr;
	static Trampoline* sub_77EBA0_t                       = nullptr;
	static Trampoline* njDrawModel_SADX_t                 = nullptr;
	static Trampoline* njDrawModel_SADX_Dynamic_t         = nullptr;
	static Trampoline* Direct3D_SetProjectionMatrix_t     = nullptr;
	static Trampoline* Direct3D_SetViewportAndTransform_t = nullptr;
	static Trampoline* Direct3D_SetWorldTransform_t       = nullptr;
	static Trampoline* CreateDirect3DDevice_t             = nullptr;
	static Trampoline* PolyBuff_DrawTriangleStrip_t       = nullptr;
	static Trampoline* PolyBuff_DrawTriangleList_t        = nullptr;

	static HRESULT __stdcall DrawPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT StartVertex,
		UINT PrimitiveCount);
	static HRESULT __stdcall DrawIndexedPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		INT BaseVertexIndex,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT startIndex,
		UINT primCount);
	static HRESULT __stdcall DrawPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT PrimitiveCount,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride);
	static HRESULT __stdcall DrawIndexedPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT PrimitiveCount,
		CONST void* pIndexData,
		D3DFORMAT IndexDataFormat,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride);

	static decltype(DrawPrimitive_r)*          DrawPrimitive_t          = nullptr;
	static decltype(DrawIndexedPrimitive_r)*   DrawIndexedPrimitive_t   = nullptr;
	static decltype(DrawPrimitiveUP_r)*        DrawPrimitiveUP_t        = nullptr;
	static decltype(DrawIndexedPrimitiveUP_r)* DrawIndexedPrimitiveUP_t = nullptr;

	constexpr auto COMPILER_FLAGS = D3DXSHADER_PACKMATRIX_ROWMAJOR | D3DXSHADER_OPTIMIZATION_LEVEL3;

	constexpr auto DEFAULT_FLAGS = ShaderFlags_Alpha | ShaderFlags_Fog | ShaderFlags_Light | ShaderFlags_Specular | ShaderFlags_Texture;
	constexpr auto VS_FLAGS = ShaderFlags_Texture | ShaderFlags_EnvMap;
	constexpr auto PS_FLAGS = ShaderFlags_Texture | ShaderFlags_Alpha | ShaderFlags_Fog | ShaderFlags_Light | ShaderFlags_Specular;

	static Uint32 shader_flags = DEFAULT_FLAGS;
	static Uint32 last_flags = DEFAULT_FLAGS;

	static std::vector<uint8_t> shader_file;
	static std::unordered_map<ShaderFlags, VertexShader> vertex_shaders;
	static std::unordered_map<ShaderFlags, PixelShader> pixel_shaders;

	static bool initialized = false;
	static Uint32 drawing = 0;
	static bool using_shader = false;
	static std::vector<D3DXMACRO> macros;

	DataPointer(Direct3DDevice8*, Direct3D_Device, 0x03D128B0);
	DataPointer(D3DXMATRIX, TransformationMatrix, 0x03D0FD80);
	DataPointer(D3DXMATRIX, ViewMatrix, 0x0389D398);
	DataPointer(D3DXMATRIX, WorldMatrix, 0x03D12900);
	DataPointer(D3DXMATRIX, _ProjectionMatrix, 0x03D129C0);
	DataPointer(int, TransformAndViewportInvalid, 0x03D0FD1C);

	static auto sanitize(Uint32& flags)
	{
		flags &= ShaderFlags_Mask;

		if (flags & ShaderFlags_EnvMap && !(flags & ShaderFlags_Texture))
		{
			flags &= ~ShaderFlags_EnvMap;
		}

		return flags;
	}

	static void free_shaders()
	{
		vertex_shaders.clear();
		pixel_shaders.clear();
		d3d::vertex_shader = nullptr;
		d3d::pixel_shader = nullptr;
	}

	static void clear_shaders()
	{
		shader_file.clear();
		free_shaders();
	}

	static VertexShader get_vertex_shader(Uint32 flags);
	static PixelShader get_pixel_shader(Uint32 flags);

	static void create_shaders()
	{
		try
		{
			d3d::vertex_shader = get_vertex_shader(DEFAULT_FLAGS);
			d3d::pixel_shader = get_pixel_shader(DEFAULT_FLAGS);

		#ifdef PRECOMPILE_SHADERS
			for (Uint32 i = 0; i < ShaderFlags_Count; i++)
			{
				auto flags = i;
				local::sanitize(flags);

				auto vs = static_cast<ShaderFlags>(flags & VS_FLAGS);
				if (vertex_shaders.find(vs) == vertex_shaders.end())
				{
					get_vertex_shader(flags);
				}

				auto ps = static_cast<ShaderFlags>(flags & PS_FLAGS);
				if (pixel_shaders.find(ps) == pixel_shaders.end())
				{
					get_pixel_shader(flags);
				}
			}
		#endif

			for (auto& i : param::parameters)
			{
				i->commit_now(d3d::device);
			}
		}
		catch (std::exception& ex)
		{
			d3d::vertex_shader = nullptr;
			d3d::pixel_shader = nullptr;
			MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
		}
	}

	static std::string to_string(Uint32 flags)
	{
		bool thing = false;
		std::stringstream result;

		while (flags != 0)
		{
			using namespace d3d;

			if (thing)
			{
				result << " | ";
			}

			if (flags & ShaderFlags_Fog)
			{
				flags &= ~ShaderFlags_Fog;
				result << "USE_FOG";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Light)
			{
				flags &= ~ShaderFlags_Light;
				result << "USE_LIGHT";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Specular)
			{
				flags &= ~ShaderFlags_Specular;
				result << "USE_SPECULAR";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Alpha)
			{
				flags &= ~ShaderFlags_Alpha;
				result << "USE_ALPHA";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_EnvMap)
			{
				flags &= ~ShaderFlags_EnvMap;
				result << "USE_ENVMAP";
				thing = true;
				continue;
			}

			if (flags & ShaderFlags_Texture)
			{
				flags &= ~ShaderFlags_Texture;
				result << "USE_TEXTURE";
				thing = true;
				continue;
			}

			break;
		}

		return result.str();
	}

	static void create_cache()
	{
		if (!filesystem::create_directory(globals::cache_path))
		{
			throw std::exception("Failed to create cache directory!");
		}
	}

	static void invalidate_cache()
	{
		if (filesystem::exists(globals::cache_path))
		{
			if (!filesystem::remove_all(globals::cache_path))
			{
				throw std::runtime_error("Failed to delete cache directory!");
			}
		}

		create_cache();
	}

	static auto shader_hash()
	{
		HCRYPTPROV hProv = 0;
		if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
		{
			throw std::runtime_error("CryptAcquireContext failed.");
		}

		HCRYPTHASH hHash = 0;
		if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
		{
			CryptReleaseContext(hProv, 0);
			throw std::runtime_error("CryptCreateHash failed.");
		}

		try
		{
			if (!CryptHashData(hHash, shader_file.data(), shader_file.size(), 0)
				|| !CryptHashData(hHash, reinterpret_cast<const BYTE*>(&COMPILER_FLAGS), sizeof(COMPILER_FLAGS), 0))
			{
				throw std::runtime_error("CryptHashData failed.");
			}

			// temporary
			DWORD buffer_size = sizeof(size_t);
			// actual size
			DWORD hash_size = 0;

			if (!CryptGetHashParam(hHash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hash_size), &buffer_size, 0))
			{
				throw std::runtime_error("CryptGetHashParam failed while asking for hash buffer size.");
			}

			std::vector<uint8_t> result(hash_size);

			if (!CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &hash_size, 0))
			{
				throw std::runtime_error("CryptGetHashParam failed while asking for hash value.");
			}

			CryptDestroyHash(hHash);
			CryptReleaseContext(hProv, 0);
			return move(result);
		}
		catch (std::exception&)
		{
			CryptDestroyHash(hHash);
			CryptReleaseContext(hProv, 0);
			throw;
		}
	}

	static void load_shader_file(const std::basic_string<char>& shader_path)
	{
		std::ifstream file(shader_path, std::ios::ate);
		auto size = file.tellg();
		file.seekg(0);

		if (file.is_open() && size > 0)
		{
			shader_file.resize(static_cast<size_t>(size));
			file.read(reinterpret_cast<char*>(shader_file.data()), size);
		}

		file.close();
	}

	static auto read_checksum(const std::basic_string<char>& checksum_path)
	{
		std::ifstream file(checksum_path, std::ios::ate | std::ios::binary);
		auto size = file.tellg();
		file.seekg(0);

		if (size > 256 || size < 1)
		{
			throw std::runtime_error("checksum.bin file size out of range");
		}

		std::vector<uint8_t> data(static_cast<size_t>(size));
		file.read(reinterpret_cast<char*>(data.data()), data.size());
		file.close();

		return move(data);
	}

	static void store_checksum(const std::vector<uint8_t>& current_hash, const std::basic_string<char>& checksum_path)
	{
		invalidate_cache();

		std::ofstream file(checksum_path, std::ios::binary | std::ios::out);

		if (!file.is_open())
		{
			std::string error = "Failed to open file for writing: " + checksum_path;
			throw std::exception(error.c_str());
		}

		file.write(reinterpret_cast<const char*>(current_hash.data()), current_hash.size());
		file.close();
	}

	static auto shader_id(Uint32 flags)
	{
		std::stringstream result;

		result << std::hex
			<< std::setw(2)
			<< std::setfill('0')
			<< flags;

		return move(result.str());
	}

	static void populate_macros(Uint32 flags)
	{
	//#define USE_SMOOTH_LIGHTING

	#ifdef USE_SMOOTH_LIGHTING
		macros.push_back({ "USE_SMOOTH_LIGHTING", "1" });
	#endif

		while (flags != 0)
		{
			using namespace d3d;

			if (flags & ShaderFlags_Texture)
			{
				flags &= ~ShaderFlags_Texture;
				macros.push_back({ "USE_TEXTURE", "1" });
				continue;
			}

			if (flags & ShaderFlags_EnvMap)
			{
				flags &= ~ShaderFlags_EnvMap;
				macros.push_back({ "USE_ENVMAP", "1" });
				continue;
			}

			if (flags & ShaderFlags_Light)
			{
				flags &= ~ShaderFlags_Light;
				macros.push_back({ "USE_LIGHT", "1" });
				continue;
			}

			if (flags & ShaderFlags_Specular)
			{
				flags &= ~ShaderFlags_Specular;
				macros.push_back({ "USE_SPECULAR", "1" });
				continue;
			}

			if (flags & ShaderFlags_Alpha)
			{
				flags &= ~ShaderFlags_Alpha;
				macros.push_back({ "USE_ALPHA", "1" });
				continue;
			}

			if (flags & ShaderFlags_Fog)
			{
				flags &= ~ShaderFlags_Fog;
				macros.push_back({ "USE_FOG", "1" });
				continue;
			}

			break;
		}

		macros.push_back({});
	}

	static __declspec(noreturn) void d3d_exception(Buffer buffer, HRESULT code)
	{
		using namespace std;

		stringstream message;

		message << '['
			<< hex
			<< setw(8)
			<< setfill('0')
			<< code;

		message << "] ";

		if (buffer != nullptr)
		{
			message << reinterpret_cast<const char*>(buffer->GetBufferPointer());
		}
		else
		{
			message << "Unspecified error.";
		}

		throw runtime_error(message.str());
	}

	static void check_shader_cache()
	{
		load_shader_file(globals::shader_path);

		const std::string checksum_path(move(filesystem::combine_path(globals::cache_path, "checksum.bin")));
		const std::vector<uint8_t> current_hash(shader_hash());

		if (filesystem::exists(globals::cache_path))
		{
			if (!filesystem::exists(checksum_path))
			{
				store_checksum(current_hash, checksum_path);
			}
			else
			{
				const std::vector<uint8_t> last_hash(read_checksum(checksum_path));

				if (last_hash != current_hash)
				{
					store_checksum(current_hash, checksum_path);
				}
			}
		}
		else
		{
			store_checksum(current_hash, checksum_path);
		}
	}

	static void load_cached_shader(const std::string& sid_path, std::vector<uint8_t>& data)
	{
		std::ifstream file(sid_path, std::ios_base::ate | std::ios_base::binary);
		auto size = file.tellg();
		file.seekg(0);

		if (size < 1)
		{
			throw std::runtime_error("corrupt vertex shader cache");
		}

		data.resize(static_cast<size_t>(size));
		file.read(reinterpret_cast<char*>(data.data()), data.size());
	}

	static void save_cached_shader(const std::string& sid_path, std::vector<uint8_t>& data)
	{
		std::ofstream file(sid_path, std::ios_base::binary);

		if (!file.is_open())
		{
			throw std::runtime_error("Failed to open file for cache storage.");
		}

		file.write(reinterpret_cast<char*>(data.data()), data.size());
	}

	static VertexShader get_vertex_shader(Uint32 flags)
	{
		using namespace std;

		sanitize(flags);
		flags &= VS_FLAGS;

		if (shader_file.empty())
		{
			check_shader_cache();
		}
		else
		{
			const auto it = vertex_shaders.find(static_cast<ShaderFlags>(flags));
			if (it != vertex_shaders.end())
			{
				return it->second;
			}
		}

		macros.clear();

		const string sid_path(move(filesystem::combine_path(globals::cache_path, shader_id(flags) + ".vs")));
		bool is_cached = filesystem::exists(sid_path);

		vector<uint8_t> data;

		if (is_cached)
		{
			PrintDebug("[lantern] Loading cached vertex shader #%02d: %08X (%s)\n",
				vertex_shaders.size(), flags, to_string(flags).c_str());

			load_cached_shader(sid_path, data);
		}
		else
		{
			PrintDebug("[lantern] Compiling vertex shader #%02d: %08X (%s)\n",
				vertex_shaders.size(), flags, to_string(flags).c_str());

			populate_macros(flags);

			Buffer errors;
			Buffer buffer;

			auto result = D3DXCompileShader(reinterpret_cast<char*>(shader_file.data()), shader_file.size(), macros.data(), nullptr,
				"vs_main", "vs_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

			if (FAILED(result) || errors != nullptr)
			{
				d3d_exception(errors, result);
			}

			data.resize(static_cast<size_t>(buffer->GetBufferSize()));
			memcpy(data.data(), buffer->GetBufferPointer(), data.size());
		}

		VertexShader shader;
		auto result = d3d::device->CreateVertexShader(reinterpret_cast<const DWORD*>(data.data()), &shader);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}

		if (!is_cached)
		{
			save_cached_shader(sid_path, data);
		}

		vertex_shaders[static_cast<ShaderFlags>(flags)] = shader;
		return shader;
	}

	static PixelShader get_pixel_shader(Uint32 flags)
	{
		using namespace std;

		if (shader_file.empty())
		{
			check_shader_cache();
		}
		else
		{
			const auto it = pixel_shaders.find(static_cast<ShaderFlags>(flags & PS_FLAGS));
			if (it != pixel_shaders.end())
			{
				return it->second;
			}
		}

		macros.clear();

		sanitize(flags);
		flags &= PS_FLAGS;

		const string sid_path = move(filesystem::combine_path(globals::cache_path, shader_id(flags) + ".ps"));
		bool is_cached = filesystem::exists(sid_path);

		vector<uint8_t> data;

		if (is_cached)
		{
			PrintDebug("[lantern] Loading cached pixel shader #%02d: %08X (%s)\n",
				pixel_shaders.size(), flags, to_string(flags).c_str());

			load_cached_shader(sid_path, data);
		}
		else
		{
			PrintDebug("[lantern] Compiling pixel shader #%02d: %08X (%s)\n",
				pixel_shaders.size(), flags, to_string(flags).c_str());

			populate_macros(flags);

			Buffer errors;
			Buffer buffer;

			auto result = D3DXCompileShader(reinterpret_cast<char*>(shader_file.data()), shader_file.size(), macros.data(), nullptr,
				"ps_main", "ps_3_0", COMPILER_FLAGS, &buffer, &errors, nullptr);

			if (FAILED(result) || errors != nullptr)
			{
				d3d_exception(errors, result);
			}

			data.resize(static_cast<size_t>(buffer->GetBufferSize()));
			memcpy(data.data(), buffer->GetBufferPointer(), data.size());
		}

		PixelShader shader;
		auto result = d3d::device->CreatePixelShader(reinterpret_cast<const DWORD*>(data.data()), &shader);

		if (FAILED(result))
		{
			d3d_exception(nullptr, result);
		}

		if (!is_cached)
		{
			save_cached_shader(sid_path, data);
		}

		pixel_shaders[static_cast<ShaderFlags>(flags & PS_FLAGS)] = shader;
		return shader;
	}

	static void begin()
	{
		++drawing;
	}

	static void end()
	{
		if (drawing > 0 && --drawing < 1)
		{
			drawing = 0;
			d3d::do_effect = false;
		}
	}

	static void shader_end()
	{
		if (using_shader)
		{
			d3d::device->SetPixelShader(nullptr);
			d3d::device->SetVertexShader(nullptr);
			using_shader = false;
		}
	}

	static void shader_start()
	{
		if (!d3d::do_effect || !drawing)
		{
			shader_end();
			return;
		}

		if (Camera_Data1)
		{
			param::CameraPosition = *reinterpret_cast<D3DXVECTOR3*>(&Camera_Data1->Position);
		}

		DWORD specular;
		d3d::device->GetRenderState(D3DRS_SPECULARENABLE, &specular);
		d3d::set_flags(ShaderFlags_Specular, specular == TRUE);

		bool changes = false;

		// The value here is copied so that UseBlend can be safely removed
		// when possible without permanently removing it. It's required by
		// Sky Deck, and it's only added to the flags once on stage load.
		auto flags = shader_flags;
		sanitize(flags);

		if (flags != last_flags)
		{
			VertexShader vs;
			PixelShader ps;

			changes = true;
			last_flags = flags;

			try
			{
				vs = get_vertex_shader(flags);
				ps = get_pixel_shader(flags);
			}
			catch (std::exception& ex)
			{
				shader_end();
				MessageBoxA(WindowHandle, ex.what(), "Shader creation failed", MB_OK | MB_ICONERROR);
				return;
			}

			if (!using_shader || vs != d3d::vertex_shader)
			{
				d3d::vertex_shader = vs;
				d3d::device->SetVertexShader(d3d::vertex_shader);
			}

			if (!using_shader || ps != d3d::pixel_shader)
			{
				d3d::pixel_shader = ps;
				d3d::device->SetPixelShader(d3d::pixel_shader);
			}
		}
		else if (!using_shader)
		{
			d3d::device->SetVertexShader(d3d::vertex_shader);
			d3d::device->SetPixelShader(d3d::pixel_shader);
		}

		if (changes || !IShaderParameter::values_assigned.empty())
		{
			for (auto& it : IShaderParameter::values_assigned)
			{
				it->commit(d3d::device);
			}

			IShaderParameter::values_assigned.clear();
		}

		using_shader = true;
	}

	static void hook_vtable()
	{
		enum
		{
			IndexOf_SetTexture = 65,
			IndexOf_DrawPrimitive = 81,
			IndexOf_DrawIndexedPrimitive,
			IndexOf_DrawPrimitiveUP,
			IndexOf_DrawIndexedPrimitiveUP
		};

		auto vtbl = (void**)(*(void**)d3d::device);

	#define HOOK(NAME) \
	MH_CreateHook(vtbl[IndexOf_ ## NAME], NAME ## _r, (LPVOID*)& ## NAME ## _t)

		HOOK(DrawPrimitive);
		HOOK(DrawIndexedPrimitive);
		HOOK(DrawPrimitiveUP);
		HOOK(DrawIndexedPrimitiveUP);

		MH_EnableHook(MH_ALL_HOOKS);
	}

#pragma region Trampolines

	template<typename T, typename... Args>
	static void run_trampoline(const T& original, Args... args)
	{
		begin();
		original(args...);
		end();
	}

	static void __cdecl sub_77EAD0_r(void* a1, int a2, int a3)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(sub_77EAD0), a1, a2, a3);
		end();
	}

	static void __cdecl sub_77EBA0_r(void* a1, int a2, int a3)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(sub_77EBA0), a1, a2, a3);
		end();
	}

	static void __cdecl njDrawModel_SADX_r(NJS_MODEL_SADX* a1)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX), a1);
		end();
	}

	static void __cdecl njDrawModel_SADX_Dynamic_r(NJS_MODEL_SADX* a1)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(njDrawModel_SADX_Dynamic), a1);
		end();
	}

	static void __fastcall PolyBuff_DrawTriangleStrip_r(PolyBuff* _this)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(PolyBuff_DrawTriangleStrip), _this);
		end();
	}

	static void __fastcall PolyBuff_DrawTriangleList_r(PolyBuff* _this)
	{
		begin();
		run_trampoline(TARGET_DYNAMIC(PolyBuff_DrawTriangleList), _this);
		end();
	}

	// ReSharper disable once CppDeclaratorNeverUsed
	static void __cdecl CreateDirect3DDevice_c(int behavior, int type)
	{
		auto orig = CreateDirect3DDevice_t->Target();
		auto _type = type;

		(void)orig;
		(void)_type;

		__asm
		{
			push _type
			mov edx, behavior
			call orig
		}

		if (Direct3D_Device != nullptr && !initialized)
		{
			d3d::device = Direct3D_Device->GetProxyInterface();

			initialized = true;
			d3d::load_shader();
			hook_vtable();
		}
	}

	static void __declspec(naked) CreateDirect3DDevice_r()
	{
		__asm
		{
			push [esp + 04h] // type
			push edx // behavior

			call CreateDirect3DDevice_c

			pop edx // behavior
			add esp, 4
			retn 4
		}
	}

	static void __cdecl Direct3D_SetWorldTransform_r()
	{
		TARGET_DYNAMIC(Direct3D_SetWorldTransform)();

		param::WorldMatrix = WorldMatrix;

		auto wvMatrix = WorldMatrix * ViewMatrix;
		param::wvMatrix = wvMatrix;

		D3DXMatrixInverse(&wvMatrix, nullptr, &wvMatrix);
		D3DXMatrixTranspose(&wvMatrix, &wvMatrix);
		// The inverse transpose matrix is used for environment mapping.
		param::wvMatrixInvT = wvMatrix;
	}

	static void __stdcall Direct3D_SetProjectionMatrix_r(float hfov, float nearPlane, float farPlane)
	{
		TARGET_DYNAMIC(Direct3D_SetProjectionMatrix)(hfov, nearPlane, farPlane);

		// The view matrix can also be set here if necessary.
		param::ProjectionMatrix = _ProjectionMatrix * TransformationMatrix;
	}

	static void __cdecl Direct3D_SetViewportAndTransform_r()
	{
		const auto original = TARGET_DYNAMIC(Direct3D_SetViewportAndTransform);
		bool invalid = TransformAndViewportInvalid != 0;
		original();

		if (invalid)
		{
			param::ProjectionMatrix = _ProjectionMatrix * TransformationMatrix;
		}
	}

	DataArray(StageLightData, CurrentStageLights, 0x3ABD9F8, 4);

	static void __cdecl Direct3D_PerformLighting_r(int type)
	{
		const auto target = TARGET_DYNAMIC(Direct3D_PerformLighting);
		target(type);
		d3d::set_flags(ShaderFlags_Light, true);

		D3DLIGHT9 light {};
		d3d::device->GetLight(0, &light);
		param::LightDirection = -D3DXVECTOR3(light.Direction);

		if (type == 0)
		{
			auto& sl = CurrentStageLights[0];
			param::LightDiffuse = D3DXCOLOR(sl.diffuse[0] * sl.multiplier, sl.diffuse[1] * sl.multiplier, sl.diffuse[2] * sl.multiplier, 1.0f);
			param::LightSpecular = D3DXCOLOR(sl.specular, sl.specular, sl.specular, 0.0f);
			param::LightAmbient = D3DXCOLOR(sl.ambient[0], sl.ambient[1], sl.ambient[2], 0.0f);
		}
		else
		{
			if ((LSPalette.Flags & 2) != 0) param::LightDirection = -D3DXVECTOR3(LSPalette.Direction.x, LSPalette.Direction.y, LSPalette.Direction.z);
			param::LightDiffuse = D3DXCOLOR(min(1.0f, LSPalette.CO_R), min(1.0f, LSPalette.CO_G), min(1.0f, LSPalette.CO_B), 1.0f);
			param::LightSpecular = D3DXCOLOR(min(1.0f, LSPalette.SP_R), min(1.0f, LSPalette.SP_G), min(1.0f, LSPalette.SP_B), 1.0f);
			param::LightAmbient = D3DXCOLOR(LSPalette.AMB_R, LSPalette.AMB_G, LSPalette.AMB_B, 1.0f);
		}
	}


#define D3D_ORIG(NAME) \
	NAME ## _t

	static HRESULT __stdcall DrawPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT StartVertex,
		UINT PrimitiveCount)
	{
		shader_start();
		auto result = D3D_ORIG(DrawPrimitive)(_this, PrimitiveType, StartVertex, PrimitiveCount);
		shader_end();
		return result;
	}
	static HRESULT __stdcall DrawIndexedPrimitive_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		INT BaseVertexIndex,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT startIndex,
		UINT primCount)
	{
		shader_start();
		auto result = D3D_ORIG(DrawIndexedPrimitive)(_this, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		shader_end();
		return result;
	}
	static HRESULT __stdcall DrawPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT PrimitiveCount,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride)
	{
		shader_start();
		auto result = D3D_ORIG(DrawPrimitiveUP)(_this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		shader_end();
		return result;
	}
	static HRESULT __stdcall DrawIndexedPrimitiveUP_r(IDirect3DDevice9* _this,
		D3DPRIMITIVETYPE PrimitiveType,
		UINT MinVertexIndex,
		UINT NumVertices,
		UINT PrimitiveCount,
		CONST void* pIndexData,
		D3DFORMAT IndexDataFormat,
		CONST void* pVertexStreamZeroData,
		UINT VertexStreamZeroStride)
	{
		shader_start();
		auto result = D3D_ORIG(DrawIndexedPrimitiveUP)(_this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		shader_end();
		return result;
	}

	// ReSharper disable once CppDeclaratorNeverUsed
	static void __stdcall DrawMeshSetBuffer_c(MeshSetBuffer* buffer)
	{
		if (!buffer->FVF)
		{
			return;
		}

		Direct3D_Device->SetVertexShader(buffer->FVF);
		Direct3D_Device->SetStreamSource(0, buffer->VertexBuffer, buffer->Size);

		const auto index_buffer = buffer->IndexBuffer;
		if (index_buffer)
		{
			Direct3D_Device->SetIndices(index_buffer, 0);

			begin();

			Direct3D_Device->DrawIndexedPrimitive(
				buffer->PrimitiveType,
				buffer->MinIndex,
				buffer->NumVertecies,
				buffer->StartIndex,
				buffer->PrimitiveCount);
		}
		else
		{
			begin();

			Direct3D_Device->DrawPrimitive(
				buffer->PrimitiveType,
				buffer->StartIndex,
				buffer->PrimitiveCount);
		}

		end();
	}

	// ReSharper disable once CppDeclaratorNeverUsed
	static const auto loc_77EF09 = reinterpret_cast<void*>(0x0077EF09);
	static void __declspec(naked) DrawMeshSetBuffer_asm()
	{
		__asm
		{
			push esi
			call DrawMeshSetBuffer_c
			jmp  loc_77EF09
		}
	}

	static auto __stdcall SetTransformHijack(Direct3DDevice8* _device, D3DTRANSFORMSTATETYPE type, D3DXMATRIX* matrix)
	{
		param::ProjectionMatrix = *matrix;
		return _device->SetTransform(type, matrix);
	}
#pragma endregion
}

namespace d3d
{
	IDirect3DDevice9* device = nullptr;
	VertexShader vertex_shader;
	PixelShader pixel_shader;
	bool do_effect = false;

	void load_shader()
	{
		if (!local::initialized)
		{
			return;
		}

		local::clear_shaders();
		local::create_shaders();
	}

	void set_flags(Uint32 flags, bool add)
	{
		if (add)
		{
			local::shader_flags |= flags;
		}
		else
		{
			local::shader_flags &= ~flags;
		}
	}

	bool shaders_not_null()
	{
		return vertex_shader != nullptr && pixel_shader != nullptr;
	}

	void init_trampolines()
	{
		using namespace local;

		Direct3D_PerformLighting_t         = new Trampoline(0x00412420, 0x00412426, Direct3D_PerformLighting_r);
		sub_77EAD0_t                       = new Trampoline(0x0077EAD0, 0x0077EAD7, sub_77EAD0_r);
		sub_77EBA0_t                       = new Trampoline(0x0077EBA0, 0x0077EBA5, sub_77EBA0_r);
		njDrawModel_SADX_t                 = new Trampoline(0x0077EDA0, 0x0077EDAA, njDrawModel_SADX_r);
		njDrawModel_SADX_Dynamic_t         = new Trampoline(0x00784AE0, 0x00784AE5, njDrawModel_SADX_Dynamic_r);
		Direct3D_SetProjectionMatrix_t     = new Trampoline(0x00791170, 0x00791175, Direct3D_SetProjectionMatrix_r);
		Direct3D_SetViewportAndTransform_t = new Trampoline(0x007912E0, 0x007912E8, Direct3D_SetViewportAndTransform_r);
		Direct3D_SetWorldTransform_t       = new Trampoline(0x00791AB0, 0x00791AB5, Direct3D_SetWorldTransform_r);
		CreateDirect3DDevice_t             = new Trampoline(0x00794000, 0x00794007, CreateDirect3DDevice_r);
		PolyBuff_DrawTriangleStrip_t       = new Trampoline(0x00794760, 0x00794767, PolyBuff_DrawTriangleStrip_r);
		PolyBuff_DrawTriangleList_t        = new Trampoline(0x007947B0, 0x007947B7, PolyBuff_DrawTriangleList_r);

		WriteJump(reinterpret_cast<void*>(0x0077EE45), DrawMeshSetBuffer_asm);

		// Hijacking a IDirect3DDevice8::SetTransform call in Direct3D_SetNearFarPlanes
		// to update the projection matrix.
		// This nops:
		// mov ecx, [eax] (device)
		// call dword ptr [ecx+94h] (device->SetTransform)
		WriteData<8>(reinterpret_cast<void*>(0x00403234), 0x90i8);
		WriteCall(reinterpret_cast<void*>(0x00403236), SetTransformHijack);
	}
}

extern "C"
{
	using namespace local;

	EXPORT void __cdecl OnRenderDeviceLost()
	{
		end();
		free_shaders();
	}

	EXPORT void __cdecl OnRenderDeviceReset()
	{
		create_shaders();
	}

	EXPORT void __cdecl OnExit()
	{
		param::release_parameters();
		free_shaders();
	}
}

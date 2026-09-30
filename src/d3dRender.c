#include "D3DRender.h"
#ifdef _WIN32
#include "libavformat/avformat.h"
#include <d3d9.h>
#include "d3dx9.h"
//#pragma comment(lib, "d3d9.lib")
//#pragma comment(lib, "d3dx9.lib")

typedef struct {
	D3DPRESENT_PARAMETERS d3dpp;
	IDirect3D9* m_pDirect3D9;
	IDirect3DDevice9* m_pDirect3DDevice;
	IDirect3DSurface9* m_pDirect3DSurfaceRender;
	IDirect3DSurface9* m_pBackBuffer;
	int width; int height; int format;
}_D3DRender;

typedef HRESULT WINAPI pDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex**);




D3DRender d3dRender_create(void* hwnd, int width, int height, int format)
{
	D3DFORMAT d3dFormat;
	switch (format)
	{
	case	AV_PIX_FMT_YUV420P:
		d3dFormat = (D3DFORMAT)MAKEFOURCC('Y', 'V', '1', '2');
		break;
	case	AV_PIX_FMT_NV12:
		d3dFormat = (D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2');
		break;
	case	AV_PIX_FMT_BGRA:
		d3dFormat = D3DFMT_A8R8G8B8;
		break;
	case AV_PIX_FMT_YUYV422:
		d3dFormat = D3DFMT_YUY2;
		break;
	case AV_PIX_FMT_P010:
		d3dFormat = (D3DFORMAT)MAKEFOURCC('P', '0', '1', '0');
		break;

	default:
		return NULL;
	}
	_D3DRender* _this = malloc(sizeof(_D3DRender));
	if (!_this)return 0;
	memset(_this, 0, sizeof(_D3DRender));
	_this->width = width;
	_this->height = height;
	_this->format = format;
	HRESULT lRet;
	static	HMODULE d3dlib = NULL;
	if (!d3dlib)
	{
		d3dlib = LoadLibrary(L"d3d9.dll");
	}
	if (!d3dlib)
	{
		d3dlib = LoadLibrary(L"d3d9.dll");
		goto fail;
	}
	pDirect3DCreate9Ex* createD3D = (pDirect3DCreate9Ex*)GetProcAddress(d3dlib, "Direct3DCreate9Ex");
	createD3D(D3D_SDK_VERSION, &_this->m_pDirect3D9);
	//_this->m_pDirect3D9 = Direct3DCreate9(D3D_SDK_VERSION);
	if (_this->m_pDirect3D9 == NULL)
	{
		goto fail;
	}

	_this->d3dpp.Windowed = TRUE;
	_this->d3dpp.hDeviceWindow = hwnd;
	_this->d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	_this->d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
	_this->d3dpp.EnableAutoDepthStencil = FALSE;
	_this->d3dpp.Flags = D3DPRESENTFLAG_VIDEO;
	_this->d3dpp.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
	_this->d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	D3DCAPS9 caps;
	DWORD BehaviorFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
	HRESULT hr = IDirect3D9_GetDeviceCaps(_this->m_pDirect3D9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
	if (SUCCEEDED(hr))
	{
		if (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)
		{
			BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
		}
		else
		{
			BehaviorFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
		}
	}

	lRet = IDirect3D9_CreateDevice(_this->m_pDirect3D9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, BehaviorFlags, &_this->d3dpp, &_this->m_pDirect3DDevice);
	if (FAILED(lRet))
	{
		goto fail;
	}

	lRet = IDirect3DDevice9_CreateOffscreenPlainSurface(_this->m_pDirect3DDevice, width, height, d3dFormat, D3DPOOL_DEFAULT, &_this->m_pDirect3DSurfaceRender, NULL);
	if (FAILED(lRet))
	{
		goto fail;
	}
	return _this;
fail:
	d3dRender_destory(_this);
	return NULL;
}

void d3dRender_destory(D3DRender p)
{
	_D3DRender* _this = p;
	if (_this->m_pBackBuffer)
	{
		IDirect3DSurface9_Release(_this->m_pBackBuffer);
		_this->m_pBackBuffer = NULL;
	}
	if (_this->m_pDirect3DSurfaceRender)
	{
		IDirect3DSurface9_Release(_this->m_pDirect3DSurfaceRender);
		_this->m_pDirect3DSurfaceRender = NULL;
	}
	if (_this->m_pDirect3DDevice)
	{
		IDirect3DDevice9_Release(_this->m_pDirect3DDevice);
		_this->m_pDirect3DDevice = NULL;
	}
	if (_this->m_pDirect3D9)
	{
		IDirect3D9_Release(_this->m_pDirect3D9);
		_this->m_pDirect3D9 = NULL;
	}
	free(_this);
}

int d3dRender_present(D3DRender p, unsigned char* data[8], int linesize[8])
{
	_D3DRender* _this = p;
	HRESULT lRet;
	D3DLOCKED_RECT d3d_rect;
	lRet = IDirect3DSurface9_LockRect(_this->m_pDirect3DSurfaceRender, &d3d_rect, NULL, D3DLOCK_DONOTWAIT);
	if (FAILED(lRet))
	{
		return -1;
	}

	byte* pDest = (BYTE*)d3d_rect.pBits;
	int lPitch = d3d_rect.Pitch;
	unsigned long i = 0;
	int width = _this->width;
	int height = _this->height;
	switch (_this->format)
	{
	case AV_PIX_FMT_YUV420P://yu12
	{
		uint8_t* t = data[1];
		data[1] = data[2];
		data[2] = t;
		if (linesize[0] == d3d_rect.Pitch)
		{
			int ySize = linesize[0] * (int)height;
			int uSize = linesize[1] * (int)height / 2;
			int vSize = linesize[2] * (int)height / 2;
			memcpy(pDest, data[0], ySize);
			pDest += ySize;
			memcpy(pDest, data[1], uSize);
			pDest += uSize;
			memcpy(pDest, data[2], vSize);
		}
		else
		{
			int dataSize = d3d_rect.Pitch < linesize[0] ? d3d_rect.Pitch : linesize[0];
			for (int i = 0; i < height; i++)
			{
				memcpy(pDest, data[0] + i * linesize[0], dataSize);
				pDest += d3d_rect.Pitch;
			}
			dataSize = d3d_rect.Pitch < linesize[1] ? d3d_rect.Pitch : linesize[1];
			for (int i = 0; i < height / 2; i++)
			{
				memcpy(pDest, data[1] + i * linesize[1], dataSize);
				pDest += d3d_rect.Pitch / 2;
			}

			dataSize = d3d_rect.Pitch < linesize[2] ? d3d_rect.Pitch : linesize[2];
			for (int i = 0; i < height / 2; i++)
			{
				memcpy(pDest, data[2] + i * linesize[2], dataSize);
				pDest += d3d_rect.Pitch / 2;
			}
		}
		t = data[1];
		data[1] = data[2];
		data[2] = t;
	}
	break;
	case AV_PIX_FMT_NV12://nv12
	{
		if (linesize[0] == d3d_rect.Pitch)
		{
			int ySize = linesize[0] * (int)height;
			int uvSize = linesize[1] * (int)height / 2;
			memcpy(pDest, data[0], ySize);
			pDest += ySize;
			memcpy(pDest, data[1], uvSize);
		}
		else
		{
			int dataSize = d3d_rect.Pitch < linesize[0] ? d3d_rect.Pitch : linesize[0];
			for (int i = 0; i < height; i++)
			{
				memcpy(pDest, data[0] + i * linesize[0], dataSize);
				pDest += d3d_rect.Pitch;
			}

			dataSize = d3d_rect.Pitch < linesize[1] ? d3d_rect.Pitch : linesize[1];
			for (int i = 0; i < height / 2; i++)
			{
				memcpy(pDest, data[1] + i * linesize[1], dataSize);
				pDest += d3d_rect.Pitch;
			}
		}
	}
	break;
	case  AV_PIX_FMT_YUYV422:
	case  AV_PIX_FMT_UYVY422:
	case AV_PIX_FMT_RGB555:
	case AV_PIX_FMT_BAYER_RGGB16:
	case AV_PIX_FMT_BGR24:
	case AV_PIX_FMT_ARGB:
	case AV_PIX_FMT_BGRA:
	case AV_PIX_FMT_ABGR:
	case AV_PIX_FMT_RGBA:
	default:
		if (linesize[0] == d3d_rect.Pitch)
		{
			memcpy(pDest, data[0], linesize[0] * (int)height);
		}
		else
		{
			int dataSize = d3d_rect.Pitch < linesize[0] ? d3d_rect.Pitch : linesize[0];
			for (int i = 0; i < height; i++)
			{
				memcpy(pDest, data[0] + i * linesize[0], dataSize);
				pDest += d3d_rect.Pitch;
			}
		}
		break;
	}

	lRet = IDirect3DSurface9_UnlockRect(_this->m_pDirect3DSurfaceRender);
	if (FAILED(lRet))
	{
		return -1;
	}

	lRet = IDirect3DDevice9_Clear(_this->m_pDirect3DDevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	if (FAILED(lRet))
	{
		return -1;
	}
	lRet = IDirect3DDevice9_BeginScene(_this->m_pDirect3DDevice);
	if (FAILED(lRet))
	{
		return -1;
	}
	lRet = IDirect3DDevice9_GetBackBuffer(_this->m_pDirect3DDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &_this->m_pBackBuffer);
	if (FAILED(lRet))
	{
		return -1;
	}
	lRet = IDirect3DDevice9_StretchRect(_this->m_pDirect3DDevice, _this->m_pDirect3DSurfaceRender, NULL, _this->m_pBackBuffer, NULL, D3DTEXF_LINEAR);
	if (FAILED(lRet))
	{
		return -1;
	}
	if (_this->m_pBackBuffer)
	{
		IDirect3DSurface9_Release(_this->m_pBackBuffer);
		_this->m_pBackBuffer = NULL;
	}
	lRet = IDirect3DDevice9_EndScene(_this->m_pDirect3DDevice);
	if (FAILED(lRet))
	{
		return -1;
	}
	lRet = IDirect3DDevice9_Present(_this->m_pDirect3DDevice, NULL, NULL, NULL, NULL);
	if (FAILED(lRet))
	{
		return -1;
	}
	return 0;
}
#endif
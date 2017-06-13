#include "DesktopCapture.h"

using namespace DirectX;

#include "Logging.h"
#include <dshow.h>
#include <strsafe.h>
#include <tchar.h>
#include <windows.h>
#include <dxgi.h>
#include "graphics-hook-info.h"
#include "bmem.h"
#include "dstr.h"
#include "app-helpers.h"
#include "platform.h"
#include "threading.h"
#include "obfuscate.h"
#include "nt-stuff.h"
#include "inject-library.h"
#include "DibHelper.h"
#include "window-helpers.h"
#include "ipc-util/pipe.h"
#include "libyuv/convert.h"
#include "libyuv/scale.h"
#include "CommonTypes.h"

#define STOP_BEING_BAD \
	    "This is most likely due to security software" \
        "that the Bebo Capture installation folder is excluded/ignored in the " \
        "settings of the security software you are using."

struct desktop_capture {
	struct desktop_capture_config    config;
	DWORD                         process_id;
	ipc_pipe_server_t             pipe;
};

DesktopCapture::DesktopCapture() : m_Device(nullptr),
                                   m_DeviceContext(nullptr),
                                   m_MoveSurf(nullptr),
                                   m_VertexShader(nullptr),
                                   m_PixelShader(nullptr),
                                   m_InputLayout(nullptr),
                                   m_RTV(nullptr),
                                   m_SamplerLinear(nullptr),
                                   m_DirtyVertexBufferAlloc(nullptr),
                                   m_DirtyVertexBufferAllocSize(0),
								   m_DeskDupl(nullptr),
								   m_AcquiredDesktopImage(nullptr),
								   m_MetaDataBuffer(nullptr),
								   m_MetaDataSize(0),
								   m_DXResource(new DXResources),
								   m_MouseInfo(new PtrInfo),
								   m_Initialized(false),
								   m_LastFrameData(new FrameData)
{
	RtlZeroMemory(&m_OutputDesc, sizeof(DXGI_OUTPUT_DESC));
}

//
// Destructor calls CleanRefs to destroy everything
//
DesktopCapture::~DesktopCapture()
{
    CleanRefs();

	delete m_LastFrameData;
	delete m_MouseInfo;
	delete m_DXResource;
}

void DesktopCapture::CleanRefs()
{
    if (m_DeviceContext)
    {
        m_DeviceContext->Release();
        m_DeviceContext = nullptr;
    }

    if (m_MoveSurf)
    {
        m_MoveSurf->Release();
        m_MoveSurf = nullptr;
    }

    if (m_VertexShader)
    {
        m_VertexShader->Release();
        m_VertexShader = nullptr;
    }

    if (m_PixelShader)
    {
        m_PixelShader->Release();
        m_PixelShader = nullptr;
    }

    if (m_InputLayout)
    {
        m_InputLayout->Release();
        m_InputLayout = nullptr;
    }

    if (m_SamplerLinear)
    {
        m_SamplerLinear->Release();
        m_SamplerLinear = nullptr;
    }

    if (m_RTV)
    {
        m_RTV->Release();
        m_RTV = nullptr;
    }

	if (m_DirtyVertexBufferAlloc)
	{
		delete[] m_DirtyVertexBufferAlloc;
		m_DirtyVertexBufferAlloc = nullptr;
	}

	if (m_DeskDupl)
	{
		m_DeskDupl->Release();
		m_DeskDupl = nullptr;
	}

	if (m_AcquiredDesktopImage)
	{
		m_AcquiredDesktopImage->Release();
		m_AcquiredDesktopImage = nullptr;
	}

	if (m_MetaDataBuffer)
	{
		delete[] m_MetaDataBuffer;
		m_MetaDataBuffer = nullptr;
	}

	if (m_Device)
	{
		m_Device->Release();
		m_Device = nullptr;
	}
}

//
// Initialize
//
void DesktopCapture::Init(int desktopId)
{
	m_iDesktopNumber = desktopId;
	m_Initialized = true;

	InitializeDXResources();
    m_Device = m_DXResource->Device;
    m_DeviceContext = m_DXResource->Context;
    m_VertexShader = m_DXResource->VertexShader;
    m_PixelShader = m_DXResource->PixelShader;
    m_InputLayout = m_DXResource->InputLayout;
    m_SamplerLinear = m_DXResource->SamplerLinear;

    m_Device->AddRef();
    m_DeviceContext->AddRef();
    m_VertexShader->AddRef();
    m_PixelShader->AddRef();
    m_InputLayout->AddRef();
    m_SamplerLinear->AddRef();

	CreateSurface();
	InitDupl();
}

HRESULT DesktopCapture::InitializeDXResources() {
	HRESULT hr = S_OK;

	// Driver types supported
	D3D_DRIVER_TYPE DriverTypes[] =
	{
		D3D_DRIVER_TYPE_HARDWARE,
		D3D_DRIVER_TYPE_WARP,
		D3D_DRIVER_TYPE_REFERENCE,
	};
	UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

	// Feature levels supported
	D3D_FEATURE_LEVEL FeatureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_1
	};
	UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

	D3D_FEATURE_LEVEL FeatureLevel;

	// Create device
	for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
	{
		hr = D3D11CreateDevice(nullptr, DriverTypes[DriverTypeIndex], nullptr, 0, FeatureLevels, NumFeatureLevels,
			D3D11_SDK_VERSION, &m_DXResource->Device, &FeatureLevel, &m_DXResource->Context);
		if (SUCCEEDED(hr))
		{
			// Device creation success, no need to loop anymore
			break;
		}
	}
	if (FAILED(hr))
	{
		error("Failed to create device in Initialize DX");
		return E_FAIL;
	}

	// VERTEX shader
	UINT Size = ARRAYSIZE(g_VS);
	hr = m_DXResource->Device->CreateVertexShader(g_VS, Size, nullptr, &m_DXResource->VertexShader);
	if (FAILED(hr))
	{
		error("Failed to create vertex shader in InitializeDx");
		return E_FAIL;
	}

	// Input layout
	D3D11_INPUT_ELEMENT_DESC Layout[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	UINT NumElements = ARRAYSIZE(Layout);
	hr = m_DXResource->Device->CreateInputLayout(Layout, NumElements, g_VS, Size, &m_DXResource->InputLayout);
	if (FAILED(hr))
	{
		error("Failed to create input layout in Initialize DX");
		return E_FAIL;
	}
	m_DXResource->Context->IASetInputLayout(m_DXResource->InputLayout);

	// Pixel shader
	Size = ARRAYSIZE(g_PS);
	hr = m_DXResource->Device->CreatePixelShader(g_PS, Size, nullptr, &m_DXResource->PixelShader);
	if (FAILED(hr))
	{
		error("Failed to create pixel shader in Initialize DX");
		return E_FAIL;
	}

	// Set up sampler
	D3D11_SAMPLER_DESC SampDesc;
	RtlZeroMemory(&SampDesc, sizeof(SampDesc));
	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	SampDesc.MinLOD = 0;
	SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = m_DXResource->Device->CreateSamplerState(&SampDesc, &m_DXResource->SamplerLinear);
	if (FAILED(hr))
	{
		error("Failed to create sampler state in Initialize DX");
		return E_FAIL;
	}

	return hr;
}

HRESULT DesktopCapture::CreateSurface() {
	HRESULT hr;

	IDXGIDevice* DxgiDevice = nullptr;
	hr = m_DXResource->Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
	if (FAILED(hr))
	{
		error("Failed to QI for DXGI Device");
		return hr;
	}

	IDXGIAdapter* DxgiAdapter = nullptr;
	hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
	DxgiDevice->Release();
	DxgiDevice = nullptr;
	if (FAILED(hr))
	{
		error("Failed to get parent DXGI Adapter");
		return hr;
	}

	IDXGIOutput* DxgiOutput = nullptr;

	// Figure out right dimensions for full size desktop texture and # of outputs to duplicate
	hr = DxgiAdapter->EnumOutputs(m_iDesktopNumber, &DxgiOutput);
	if (FAILED(hr))
	{
		DxgiAdapter->Release();
		DxgiAdapter = nullptr;
		error("Output specified to be duplicated does not exist");
		return hr;
	}

	RtlZeroMemory(&m_OutputDesc, sizeof(DXGI_OUTPUT_DESC));
	DxgiOutput->GetDesc(&m_OutputDesc);

	DxgiOutput->Release();
	DxgiOutput = nullptr;

	DxgiAdapter->Release();
	DxgiAdapter = nullptr;

	D3D11_TEXTURE2D_DESC CopyBufferDesc;
	CopyBufferDesc.Width = m_OutputDesc.DesktopCoordinates.right - m_OutputDesc.DesktopCoordinates.left;
	CopyBufferDesc.Height = m_OutputDesc.DesktopCoordinates.bottom - m_OutputDesc.DesktopCoordinates.top;
	CopyBufferDesc.MipLevels = 1;
	CopyBufferDesc.ArraySize = 1;
	CopyBufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	CopyBufferDesc.SampleDesc.Count = 1;
	CopyBufferDesc.SampleDesc.Quality = 0;
	CopyBufferDesc.Usage = D3D11_USAGE_STAGING;
	CopyBufferDesc.BindFlags = 0;
	CopyBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CopyBufferDesc.MiscFlags = 0;

	hr = m_DXResource->Device->CreateTexture2D(&CopyBufferDesc, nullptr, &m_CopyBuffer);
	if (FAILED(hr))
	{
		error("Failed to create staging texture for pointer");
		return hr;
	}

	hr = m_CopyBuffer->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(&m_Surface));
	if (FAILED(hr))
	{
		error("Failed to QI for IDXGI Surface");
		return hr;
	}
	
	return hr;
}

HRESULT DesktopCapture::InitDupl() {
	// Get DXGI device
	IDXGIDevice* DxgiDevice = nullptr;
	HRESULT hr = m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&DxgiDevice));
	if (FAILED(hr))
	{
		error("Failed to QI for DXGI Device");
		return E_FAIL;
	}

	// Get DXGI adapter
	IDXGIAdapter* DxgiAdapter = nullptr;
	hr = DxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&DxgiAdapter));
	DxgiDevice->Release();
	DxgiDevice = nullptr;
	if (FAILED(hr))
	{
		error("Failed to get parent DXGI Adapter");
		return E_FAIL;
	}

	// Get output
	IDXGIOutput* DxgiOutput = nullptr;
	hr = DxgiAdapter->EnumOutputs(m_iDesktopNumber, &DxgiOutput);
	DxgiAdapter->Release();
	DxgiAdapter = nullptr;
	if (FAILED(hr))
	{
		error("Failed to get specific output on DXGI Output");
		return E_FAIL;
	}

	memset(&m_OutputDesc, 0, sizeof(m_OutputDesc));
	DxgiOutput->GetDesc(&m_OutputDesc);

	// QI for Output 1
	IDXGIOutput1* DxgiOutput1 = nullptr;
	hr = DxgiOutput->QueryInterface(__uuidof(DxgiOutput1), reinterpret_cast<void**>(&DxgiOutput1));
	DxgiOutput->Release();
	DxgiOutput = nullptr;
	if (FAILED(hr))
	{
		error("Failed to QI for DxgiOutput1 in DesktopCapture");
		return E_FAIL;
	}

	// Create desktop duplication
	hr = DxgiOutput1->DuplicateOutput(m_Device, &m_DeskDupl);
	DxgiOutput1->Release();
	DxgiOutput1 = nullptr;
	if (FAILED(hr))
	{
		if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
		{
			error("There is already the maximum number of applications using the Desktop Duplication API running, please close one of those applications and then try again.");
			return E_FAIL;
		}
		error("Failed to get duplicate output in DesktopCapture");
		return E_FAIL;
	}

	return S_OK;
}



//
// Process a given frame and its metadata
//
HRESULT DesktopCapture::ProcessFrame(FrameData* Data, ID3D11Texture2D* SharedSurf, INT OffsetX, INT OffsetY, _In_ DXGI_OUTPUT_DESC* DeskDesc)
{
    HRESULT Ret = DUPL_RETURN_SUCCESS;

    // Process dirties and moves
    if (Data->FrameInfo.TotalMetadataBufferSize)
    {
        D3D11_TEXTURE2D_DESC Desc;
        Data->Frame->GetDesc(&Desc);

        if (Data->MoveCount)
        {
            Ret = CopyMove(SharedSurf, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(Data->MetaData), Data->MoveCount, OffsetX, OffsetY, DeskDesc, Desc.Width, Desc.Height);
            if (Ret != DUPL_RETURN_SUCCESS)
            {
                return Ret;
            }
        }

        if (Data->DirtyCount)
        {
            Ret = CopyDirty(Data->Frame, SharedSurf, reinterpret_cast<RECT*>(Data->MetaData + (Data->MoveCount * sizeof(DXGI_OUTDUPL_MOVE_RECT))), Data->DirtyCount, OffsetX, OffsetY, DeskDesc);
        }
    }

    return Ret;
}

//
// Set appropriate source and destination rects for move rects
//
void DesktopCapture::SetMoveRect(_Out_ RECT* SrcRect, _Out_ RECT* DestRect, _In_ DXGI_OUTPUT_DESC* DeskDesc, _In_ DXGI_OUTDUPL_MOVE_RECT* MoveRect, INT TexWidth, INT TexHeight)
{
    switch (DeskDesc->Rotation)
    {
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        {
            SrcRect->left = MoveRect->SourcePoint.x;
            SrcRect->top = MoveRect->SourcePoint.y;
            SrcRect->right = MoveRect->SourcePoint.x + MoveRect->DestinationRect.right - MoveRect->DestinationRect.left;
            SrcRect->bottom = MoveRect->SourcePoint.y + MoveRect->DestinationRect.bottom - MoveRect->DestinationRect.top;

            *DestRect = MoveRect->DestinationRect;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE90:
        {
            SrcRect->left = TexHeight - (MoveRect->SourcePoint.y + MoveRect->DestinationRect.bottom - MoveRect->DestinationRect.top);
            SrcRect->top = MoveRect->SourcePoint.x;
            SrcRect->right = TexHeight - MoveRect->SourcePoint.y;
            SrcRect->bottom = MoveRect->SourcePoint.x + MoveRect->DestinationRect.right - MoveRect->DestinationRect.left;

            DestRect->left = TexHeight - MoveRect->DestinationRect.bottom;
            DestRect->top = MoveRect->DestinationRect.left;
            DestRect->right = TexHeight - MoveRect->DestinationRect.top;
            DestRect->bottom = MoveRect->DestinationRect.right;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180:
        {
            SrcRect->left = TexWidth - (MoveRect->SourcePoint.x + MoveRect->DestinationRect.right - MoveRect->DestinationRect.left);
            SrcRect->top = TexHeight - (MoveRect->SourcePoint.y + MoveRect->DestinationRect.bottom - MoveRect->DestinationRect.top);
            SrcRect->right = TexWidth - MoveRect->SourcePoint.x;
            SrcRect->bottom = TexHeight - MoveRect->SourcePoint.y;

            DestRect->left = TexWidth - MoveRect->DestinationRect.right;
            DestRect->top = TexHeight - MoveRect->DestinationRect.bottom;
            DestRect->right = TexWidth - MoveRect->DestinationRect.left;
            DestRect->bottom =  TexHeight - MoveRect->DestinationRect.top;
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270:
        {
            SrcRect->left = MoveRect->SourcePoint.x;
            SrcRect->top = TexWidth - (MoveRect->SourcePoint.x + MoveRect->DestinationRect.right - MoveRect->DestinationRect.left);
            SrcRect->right = MoveRect->SourcePoint.y + MoveRect->DestinationRect.bottom - MoveRect->DestinationRect.top;
            SrcRect->bottom = TexWidth - MoveRect->SourcePoint.x;

            DestRect->left = MoveRect->DestinationRect.top;
            DestRect->top = TexWidth - MoveRect->DestinationRect.right;
            DestRect->right = MoveRect->DestinationRect.bottom;
            DestRect->bottom =  TexWidth - MoveRect->DestinationRect.left;
            break;
        }
        default:
        {
            RtlZeroMemory(DestRect, sizeof(RECT));
            RtlZeroMemory(SrcRect, sizeof(RECT));
            break;
        }
    }
}

//
// Copy move rectangles
//
HRESULT DesktopCapture::CopyMove(_Inout_ ID3D11Texture2D* SharedSurf, _In_reads_(MoveCount) DXGI_OUTDUPL_MOVE_RECT* MoveBuffer, UINT MoveCount, INT OffsetX, INT OffsetY, _In_ DXGI_OUTPUT_DESC* DeskDesc, INT TexWidth, INT TexHeight)
{
    D3D11_TEXTURE2D_DESC FullDesc;
    SharedSurf->GetDesc(&FullDesc);

    // Make new intermediate surface to copy into for moving
    if (!m_MoveSurf)
    {
        D3D11_TEXTURE2D_DESC MoveDesc;
        MoveDesc = FullDesc;
        MoveDesc.Width = DeskDesc->DesktopCoordinates.right - DeskDesc->DesktopCoordinates.left;
        MoveDesc.Height = DeskDesc->DesktopCoordinates.bottom - DeskDesc->DesktopCoordinates.top;
        MoveDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        MoveDesc.MiscFlags = 0;
        HRESULT hr = m_Device->CreateTexture2D(&MoveDesc, nullptr, &m_MoveSurf);
        if (FAILED(hr))
        {
            // return ProcessFailure(m_Device, L"Failed to create staging texture for move rects", L"Error", hr, SystemTransitionsExpectedErrors);
			return hr;
        }
    }

    for (UINT i = 0; i < MoveCount; ++i)
    {
        RECT SrcRect;
        RECT DestRect;

        SetMoveRect(&SrcRect, &DestRect, DeskDesc, &(MoveBuffer[i]), TexWidth, TexHeight);

        // Copy rect out of shared surface
        D3D11_BOX Box;
        Box.left = SrcRect.left + DeskDesc->DesktopCoordinates.left - OffsetX;
        Box.top = SrcRect.top + DeskDesc->DesktopCoordinates.top - OffsetY;
        Box.front = 0;
        Box.right = SrcRect.right + DeskDesc->DesktopCoordinates.left - OffsetX;
        Box.bottom = SrcRect.bottom + DeskDesc->DesktopCoordinates.top - OffsetY;
        Box.back = 1;
        m_DeviceContext->CopySubresourceRegion(m_MoveSurf, 0, SrcRect.left, SrcRect.top, 0, SharedSurf, 0, &Box);

        // Copy back to shared surface
        Box.left = SrcRect.left;
        Box.top = SrcRect.top;
        Box.front = 0;
        Box.right = SrcRect.right;
        Box.bottom = SrcRect.bottom;
        Box.back = 1;
        m_DeviceContext->CopySubresourceRegion(SharedSurf, 0, DestRect.left + DeskDesc->DesktopCoordinates.left - OffsetX, DestRect.top + DeskDesc->DesktopCoordinates.top - OffsetY, 0, m_MoveSurf, 0, &Box);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Sets up vertices for dirty rects for rotated desktops
//
#pragma warning(push)
#pragma warning(disable:__WARNING_USING_UNINIT_VAR) // false positives in SetDirtyVert due to tool bug

void DesktopCapture::SetDirtyVert(_Out_writes_(NUMVERTICES) Vertex* Vertices, _In_ RECT* Dirty, INT OffsetX, INT OffsetY, _In_ DXGI_OUTPUT_DESC* DeskDesc, _In_ D3D11_TEXTURE2D_DESC* FullDesc, _In_ D3D11_TEXTURE2D_DESC* ThisDesc)
{
    INT CenterX = FullDesc->Width / 2;
    INT CenterY = FullDesc->Height / 2;

    INT Width = DeskDesc->DesktopCoordinates.right - DeskDesc->DesktopCoordinates.left;
    INT Height = DeskDesc->DesktopCoordinates.bottom - DeskDesc->DesktopCoordinates.top;

    // Rotation compensated destination rect
    RECT DestDirty = *Dirty;

    // Set appropriate coordinates compensated for rotation
    switch (DeskDesc->Rotation)
    {
        case DXGI_MODE_ROTATION_ROTATE90:
        {
            DestDirty.left = Width - Dirty->bottom;
            DestDirty.top = Dirty->left;
            DestDirty.right = Width - Dirty->top;
            DestDirty.bottom = Dirty->right;

            Vertices[0].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[1].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[2].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[5].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180:
        {
            DestDirty.left = Width - Dirty->right;
            DestDirty.top = Height - Dirty->bottom;
            DestDirty.right = Width - Dirty->left;
            DestDirty.bottom = Height - Dirty->top;

            Vertices[0].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[1].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[2].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[5].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270:
        {
            DestDirty.left = Dirty->top;
            DestDirty.top = Height - Dirty->right;
            DestDirty.right = Dirty->bottom;
            DestDirty.bottom = Height - Dirty->left;

            Vertices[0].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[1].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[2].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[5].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            break;
        }
        default:
            assert(false); // drop through
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        {
            Vertices[0].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[1].TexCoord = XMFLOAT2(Dirty->left / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[2].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->bottom / static_cast<FLOAT>(ThisDesc->Height));
            Vertices[5].TexCoord = XMFLOAT2(Dirty->right / static_cast<FLOAT>(ThisDesc->Width), Dirty->top / static_cast<FLOAT>(ThisDesc->Height));
            break;
        }
    }

    // Set positions
    Vertices[0].Pos = XMFLOAT3((DestDirty.left + DeskDesc->DesktopCoordinates.left - OffsetX - CenterX) / static_cast<FLOAT>(CenterX),
                             -1 * (DestDirty.bottom + DeskDesc->DesktopCoordinates.top - OffsetY - CenterY) / static_cast<FLOAT>(CenterY),
                             0.0f);
    Vertices[1].Pos = XMFLOAT3((DestDirty.left + DeskDesc->DesktopCoordinates.left - OffsetX - CenterX) / static_cast<FLOAT>(CenterX),
                             -1 * (DestDirty.top + DeskDesc->DesktopCoordinates.top - OffsetY - CenterY) / static_cast<FLOAT>(CenterY),
                             0.0f);
    Vertices[2].Pos = XMFLOAT3((DestDirty.right + DeskDesc->DesktopCoordinates.left - OffsetX - CenterX) / static_cast<FLOAT>(CenterX),
                             -1 * (DestDirty.bottom + DeskDesc->DesktopCoordinates.top - OffsetY - CenterY) / static_cast<FLOAT>(CenterY),
                             0.0f);
    Vertices[3].Pos = Vertices[2].Pos;
    Vertices[4].Pos = Vertices[1].Pos;
    Vertices[5].Pos = XMFLOAT3((DestDirty.right + DeskDesc->DesktopCoordinates.left - OffsetX - CenterX) / static_cast<FLOAT>(CenterX),
                             -1 * (DestDirty.top + DeskDesc->DesktopCoordinates.top - OffsetY - CenterY) / static_cast<FLOAT>(CenterY),
                             0.0f);

    Vertices[3].TexCoord = Vertices[2].TexCoord;
    Vertices[4].TexCoord = Vertices[1].TexCoord;
}

#pragma warning(pop) // re-enable __WARNING_USING_UNINIT_VAR

//
// Copies dirty rectangles
//
HRESULT DesktopCapture::CopyDirty(_In_ ID3D11Texture2D* SrcSurface, _Inout_ ID3D11Texture2D* SharedSurf, _In_reads_(DirtyCount) RECT* DirtyBuffer, UINT DirtyCount, INT OffsetX, INT OffsetY, _In_ DXGI_OUTPUT_DESC* DeskDesc)
{
    HRESULT hr;

    D3D11_TEXTURE2D_DESC FullDesc;
    SharedSurf->GetDesc(&FullDesc);

    D3D11_TEXTURE2D_DESC ThisDesc;
    SrcSurface->GetDesc(&ThisDesc);

    if (!m_RTV)
    {
        hr = m_Device->CreateRenderTargetView(SharedSurf, nullptr, &m_RTV);
        if (FAILED(hr))
        {
            //return ProcessFailure(m_Device, L"Failed to create render target view for dirty rects", L"Error", hr, SystemTransitionsExpectedErrors);
			return hr;
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC ShaderDesc;
    ShaderDesc.Format = ThisDesc.Format;
    ShaderDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ShaderDesc.Texture2D.MostDetailedMip = ThisDesc.MipLevels - 1;
    ShaderDesc.Texture2D.MipLevels = ThisDesc.MipLevels;

    // Create new shader resource view
    ID3D11ShaderResourceView* ShaderResource = nullptr;
    hr = m_Device->CreateShaderResourceView(SrcSurface, &ShaderDesc, &ShaderResource);
    if (FAILED(hr))
    {
        // return ProcessFailure(m_Device, L"Failed to create shader resource view for dirty rects", L"Error", hr, SystemTransitionsExpectedErrors);
		return hr;
    }

    FLOAT BlendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    m_DeviceContext->OMSetBlendState(nullptr, BlendFactor, 0xFFFFFFFF);
    m_DeviceContext->OMSetRenderTargets(1, &m_RTV, nullptr);
    m_DeviceContext->VSSetShader(m_VertexShader, nullptr, 0);
    m_DeviceContext->PSSetShader(m_PixelShader, nullptr, 0);
    m_DeviceContext->PSSetShaderResources(0, 1, &ShaderResource);
    m_DeviceContext->PSSetSamplers(0, 1, &m_SamplerLinear);
    m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Create space for vertices for the dirty rects if the current space isn't large enough
    UINT BytesNeeded = sizeof(Vertex) * NUMVERTICES * DirtyCount;
    if (BytesNeeded > m_DirtyVertexBufferAllocSize)
    {
        if (m_DirtyVertexBufferAlloc)
        {
            delete [] m_DirtyVertexBufferAlloc;
        }

        m_DirtyVertexBufferAlloc = new (std::nothrow) BYTE[BytesNeeded];
        if (!m_DirtyVertexBufferAlloc)
        {
            m_DirtyVertexBufferAllocSize = 0;
            // return ProcessFailure(nullptr, L"Failed to allocate memory for dirty vertex buffer.", L"Error", E_OUTOFMEMORY);
			return hr;
        }

        m_DirtyVertexBufferAllocSize = BytesNeeded;
    }

    // Fill them in
    Vertex* DirtyVertex = reinterpret_cast<Vertex*>(m_DirtyVertexBufferAlloc);
    for (UINT i = 0; i < DirtyCount; ++i, DirtyVertex += NUMVERTICES)
    {
        SetDirtyVert(DirtyVertex, &(DirtyBuffer[i]), OffsetX, OffsetY, DeskDesc, &FullDesc, &ThisDesc);
    }

    // Create vertex buffer
    D3D11_BUFFER_DESC BufferDesc;
    RtlZeroMemory(&BufferDesc, sizeof(BufferDesc));
    BufferDesc.Usage = D3D11_USAGE_DEFAULT;
    BufferDesc.ByteWidth = BytesNeeded;
    BufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    BufferDesc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA InitData;
    RtlZeroMemory(&InitData, sizeof(InitData));
    InitData.pSysMem = m_DirtyVertexBufferAlloc;

    ID3D11Buffer* VertBuf = nullptr;
    hr = m_Device->CreateBuffer(&BufferDesc, &InitData, &VertBuf);
    if (FAILED(hr))
    {
        // return ProcessFailure(m_Device, L"Failed to create vertex buffer in dirty rect processing", L"Error", hr, SystemTransitionsExpectedErrors);
		return hr;
    }
    UINT Stride = sizeof(Vertex);
    UINT Offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, &VertBuf, &Stride, &Offset);

    D3D11_VIEWPORT VP;
    VP.Width = static_cast<FLOAT>(FullDesc.Width);
    VP.Height = static_cast<FLOAT>(FullDesc.Height);
    VP.MinDepth = 0.0f;
    VP.MaxDepth = 1.0f;
    VP.TopLeftX = 0.0f;
    VP.TopLeftY = 0.0f;
    m_DeviceContext->RSSetViewports(1, &VP);

    m_DeviceContext->Draw(NUMVERTICES * DirtyCount, 0);

    VertBuf->Release();
    VertBuf = nullptr;

    ShaderResource->Release();
    ShaderResource = nullptr;

    return DUPL_RETURN_SUCCESS;
}

static void pipe_log(void *param, uint8_t *data, size_t size)
{
	//	struct desktop_capture *gc = param;
	if (data && size)
		info("%s", data);
}

static inline bool init_pipe(struct desktop_capture *gc)
{
	char name[64];
	sprintf(name, "%s%lu", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	return true;
}

// unused atm
static bool start_desktop_capture(struct desktop_capture *gc)
{
	debug("Starting capture");
	return true;
}

// unused atm
static void stop_desktop_capture(struct desktop_capture *gc)
{
	ipc_pipe_server_free(&gc->pipe);
}

// unused atm
boolean stop_desktop_capture(void **data) {
	struct desktop_capture *gc = (desktop_capture *)*data;
	stop_desktop_capture(gc);
	return true;
}

static inline int getI420BufferSize(int width, int height) {
	int half_width = (width + 1) >> 1;
	int half_height = (height + 1) >> 1;
	return width * height + half_width * half_height * 2;
}

bool DesktopCapture::PushFrame(IMediaSample *pSample, DXGI_SURFACE_DESC frameDesc, DXGI_MAPPED_RECT map, int dWidth, int dHeight) {
	debug("push frame - frame: %dx%d, negotiated: %dx%d", frameDesc.Width, frameDesc.Height, dWidth, dHeight);
	if (!map.pBits) {
		warn("push frame - pBits is NULL");
		return false;
	}

	BYTE *pData;
	pSample->GetPointer(&pData);

	const uint8_t* src_frame = static_cast<uint8_t*>(map.pBits);
	int src_stride_frame = map.Pitch;

	int width = frameDesc.Width;
	int height = frameDesc.Height;

	BYTE* yuv = new BYTE[getI420BufferSize(width, height)];

	uint8* y = yuv;
	int stride_y = width;
	uint8* u = yuv + (width * height);
	int stride_u = (width + 1) / 2;
	uint8* v = u + ((width * height) >> 2);
	int stride_v = stride_u;

	libyuv::ARGBToI420(src_frame, src_stride_frame,
		y, stride_y,
		u, stride_u,
		v, stride_v,
		width, height);

	int dst_width = dWidth;
	int dst_height = dHeight;
	uint8* dst_y = pData;
	int dst_stride_y = dst_width;
	uint8* dst_u = pData + (dst_width * dst_height);
	int dst_stride_u = (dst_width + 1) / 2;
	uint8* dst_v = dst_u + ((dst_width * dst_height) >> 2);
	int dst_stride_v = dst_stride_u;

	libyuv::I420Scale(
		y, stride_y,
		u, stride_u,
		v, stride_v,
		width, height,
		dst_y, dst_stride_y,
		dst_u, dst_stride_u,
		dst_v, dst_stride_v,
		dst_width, dst_height,
		libyuv::FilterMode(libyuv::kFilterBox)
	);

	delete[] yuv;
	return true;
}

//
// Retrieves mouse info and write it into PtrInfo
//
HRESULT DesktopCapture::GetMouse(_Inout_ PtrInfo* PtrInfo, _In_ DXGI_OUTDUPL_FRAME_INFO* FrameInfo, INT OffsetX, INT OffsetY)
{
	// A non-zero mouse update timestamp indicates that there is a mouse position update and optionally a shape change
	if (FrameInfo->LastMouseUpdateTime.QuadPart == 0)
	{
		return S_OK;
	}

	bool UpdatePosition = true;

	// Make sure we don't update pointer position wrongly
	// If pointer is invisible, make sure we did not get an update from another output that the last time that said pointer
	// was visible, if so, don't set it to invisible or update.
	if (!FrameInfo->PointerPosition.Visible && (PtrInfo->WhoUpdatedPositionLast != m_iDesktopNumber))
	{
		UpdatePosition = false;
	}

	// If two outputs both say they have a visible, only update if new update has newer timestamp
	if (FrameInfo->PointerPosition.Visible && PtrInfo->Visible && (PtrInfo->WhoUpdatedPositionLast != m_iDesktopNumber) && (PtrInfo->LastTimeStamp.QuadPart > FrameInfo->LastMouseUpdateTime.QuadPart))
	{
		UpdatePosition = false;
	}

	// Update position
	if (UpdatePosition)
	{
		PtrInfo->Position.x = FrameInfo->PointerPosition.Position.x + m_OutputDesc.DesktopCoordinates.left - OffsetX;
		PtrInfo->Position.y = FrameInfo->PointerPosition.Position.y + m_OutputDesc.DesktopCoordinates.top - OffsetY;
		PtrInfo->WhoUpdatedPositionLast = m_iDesktopNumber;
		PtrInfo->LastTimeStamp = FrameInfo->LastMouseUpdateTime;
		PtrInfo->Visible = FrameInfo->PointerPosition.Visible != 0;
	}

	// No new shape
	if (FrameInfo->PointerShapeBufferSize == 0)
	{
		return S_OK;
	}

	// Old buffer too small
	if (FrameInfo->PointerShapeBufferSize > PtrInfo->BufferSize)
	{
		if (PtrInfo->PtrShapeBuffer)
		{
			delete[] PtrInfo->PtrShapeBuffer;
			PtrInfo->PtrShapeBuffer = nullptr;
		}
		PtrInfo->PtrShapeBuffer = new (std::nothrow) BYTE[FrameInfo->PointerShapeBufferSize];
		if (!PtrInfo->PtrShapeBuffer)
		{
			PtrInfo->BufferSize = 0;
			error("Failed to allocate memory for pointer shape in DesktopCapture");
			return E_UNEXPECTED;
		}

		// Update buffer size
		PtrInfo->BufferSize = FrameInfo->PointerShapeBufferSize;
	}

	// Get shape
	UINT BufferSizeRequired;
	HRESULT hr = m_DeskDupl->GetFramePointerShape(FrameInfo->PointerShapeBufferSize, reinterpret_cast<VOID*>(PtrInfo->PtrShapeBuffer), &BufferSizeRequired, &(PtrInfo->ShapeInfo));
	if (FAILED(hr))
	{
		delete[] PtrInfo->PtrShapeBuffer;
		PtrInfo->PtrShapeBuffer = nullptr;
		PtrInfo->BufferSize = 0;
		error("Failed to get frame pointer shape in DesktopCapture");
		return E_UNEXPECTED;
	}

	return S_OK;
}


bool DesktopCapture::AcquireNextFrame(DXGI_OUTDUPL_FRAME_INFO * frame, IDXGIResource ** resource) {
	HRESULT hr = S_OK;

	if (!m_DeskDupl) {
		hr = ReinitializeDuplication();
	}

	if (FAILED(hr)) {
		return false;
	}

	hr = m_DeskDupl->AcquireNextFrame(300, frame, resource);
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		error("Failed to acquire next frame - dxgi error access lost.");

		hr = ReinitializeDuplication();

		if (FAILED(hr)) {
			return false;
		}

		hr = m_DeskDupl->AcquireNextFrame(300, frame, resource);
	} else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		error("Failed to acquire next frame - timeout.");
		return false;
	} else if (FAILED(hr)) {
		error("Failed to acquire next frame in DesktopCapture - %ld", hr);
		return false;
	}

	// If still holding old frame, destroy it
	if (m_AcquiredDesktopImage)
	{
		m_AcquiredDesktopImage->Release();
		m_AcquiredDesktopImage = nullptr;
	}

	return SUCCEEDED(hr);
}

//
// Get next frame and write it into Data
//
bool DesktopCapture::GetFrame(IMediaSample *pSample, bool miss, int width, int height, bool captureMouse)
{
	if (!m_Initialized) {
		error("DesktopCapture.Init() required.");
		return false;
	}

	IDXGIResource* DesktopResource = nullptr;
	DXGI_OUTDUPL_FRAME_INFO FrameInfo = { 0 };

	bool got_frame = AcquireNextFrame(&FrameInfo, &DesktopResource);

	if (!got_frame) {
		error("Unable to acquire next frame");
		return false;
	}

	// QI for IDXGIResource
	HRESULT hr = DesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&m_AcquiredDesktopImage));
	DesktopResource->Release();
	DesktopResource = nullptr;

	if (FAILED(hr)) {
		error("Failed to QI for ID3D11Texture2D from acquired IDXGIResource in DesktopCapture");
		return false;
	}

	m_LastFrameData->Frame = m_AcquiredDesktopImage;
	m_LastFrameData->FrameInfo = FrameInfo;

	m_DXResource->Context->CopyResource(m_CopyBuffer, m_LastFrameData->Frame);

	DXGI_MAPPED_RECT Map;
	DXGI_SURFACE_DESC FrameDesc;

	m_Surface->GetDesc(&FrameDesc);

	m_Surface->Map(&Map, D3D11_MAP_READ);
	got_frame = PushFrame(pSample, FrameDesc, Map, width, height);
	m_Surface->Unmap();

	DoneWithFrame();

	return got_frame;
}

//
// Release frame
//
bool DesktopCapture::DoneWithFrame()
{
	HRESULT hr = m_DeskDupl->ReleaseFrame();
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		error("Failed to release frame, but trying to reinitialize desktop capture");

		hr = ReinitializeDuplication();
		if (FAILED(hr)) {
			error("Failed to release frame AND FAILED to reinitialize desktop capture");
		}
	} else if (FAILED(hr)) {
		error("Failed to release frame in DesktopCapture");
	}

	if (m_AcquiredDesktopImage) {
		m_AcquiredDesktopImage->Release();
		m_AcquiredDesktopImage = nullptr;
	}

	return true;
}

HRESULT DesktopCapture::ReinitializeDuplication() {
	if (m_DeskDupl) {
		m_DeskDupl->Release();
		m_DeskDupl = nullptr;
	}

	if (m_AcquiredDesktopImage) {
		m_AcquiredDesktopImage->Release();
		m_AcquiredDesktopImage = nullptr;
	}

	if (m_CopyBuffer) {
		m_CopyBuffer->Release();
		m_CopyBuffer = nullptr;
	}

	if (m_Surface) {
		m_Surface->Release();
		m_Surface = nullptr;
	}

	HRESULT hr = CreateSurface();
	if (FAILED(hr)) {
		error("Failed to create surface in reinitialization");
		return hr;
	}

	hr = InitDupl();
	if (FAILED(hr)) {
		error("Failed to init desktop duplication in reinitialization");
		return hr;
	}

	info("reinitlizing, done");
}

#pragma comment(lib,"D3D11.lib")
#pragma comment(lib,"D3dcompiler.lib")
#pragma comment(lib,"Dxgi.lib")

// Tell OpenXR which platform code we'll be using
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <d3d11.h>
#include <directxmath.h>
#include <d3dcompiler.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <thread>
#include <vector>
#include <algorithm>

using namespace std;
using namespace DirectX;


struct SwapchainInfo {
	XrSwapchain xrSwapchainHandle;
	int32_t width;
	int32_t height;
	vector<XrSwapchainImageD3D11KHR> xrSwapchainImages;
	vector<ID3D11DepthStencilView*> depthStencilViews;
	vector<ID3D11RenderTargetView*> renderTargetViews;
};


// Utilities
const XrPosef POSE_IDENTITY = { {0,0,0,1}, {0,0,0} };

inline XrPath StringToPath(XrInstance instance, const char* str) {
	XrPath path;
	xrStringToPath(instance, str, &path);
	return path;
}

// OpenXR
XrInstance xrInstance = {};
XrSession xrSession = {};
XrSessionState xrSessionState = XR_SESSION_STATE_UNKNOWN;
XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
XrEnvironmentBlendMode xrEnvironmentBlendMode = {};
const char* renderingExtension;

XrSpace xrSpace = {};
XrActionSet xrActionSet;
XrAction xrAction_HandPose;
XrAction xrAction_Select;
XrPath xrPath_HandSubactions[2];
XrSpace xrSpace_Hands[2];
XrPosef xrPosef_Hands[2];
XrBool32 xrBool_IsHandPoseActive[2];

uint32_t viewCount = 0;
vector<XrView> xrViews;
vector<XrViewConfigurationView> xrViewConfigurationViews;
vector<SwapchainInfo> SwapchainsInfo;

PFN_xrGetD3D11GraphicsRequirementsKHR ext_xrGetD3D11GraphicsRequirementsKHR = nullptr;
XrGraphicsRequirementsD3D11KHR xrGraphicsRequirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };

bool IsXrSessionRunning = false;

// Scene and Rendering

// CPU data
struct ModelConstantBuffer {
	XMFLOAT4X4 Model;
};

struct ViewProjectionConstantBuffer {
	XMFLOAT4X4 ViewProjection;
};

// GPU settings and resources
IDXGIAdapter1* graphicsAdapter = nullptr;
IDXGIFactory1* dxgiFactory;
DXGI_ADAPTER_DESC1 adapterDesc;

D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

ID3D11Device* d3dDevice = nullptr;
ID3D11DeviceContext* d3dContext = nullptr;

ID3D11VertexShader* vertexShader;
ID3D11PixelShader* pixelShader;
ID3D11InputLayout* inputLayout;
ID3D11Buffer* modelConstantBuffer;
ID3D11Buffer* viewProjectionConstantBuffer;
ID3D11Buffer* vertexBuffer;
ID3D11Buffer* indexBuffer;


constexpr char shader[] = R"_(

cbuffer ModelConstantBuffer : register(b0) 
{
	float4x4 Model;
};

cbuffer ViewProjectionConstantBuffer : register(b1) 
{
	float4x4 ViewProjection;
};

struct VertexShaderInput 
{
	float4 pos   : SV_POSITION;
	float3 color : COLOR0;
};

struct VertexShaderOutput 
{
	float4 pos   : SV_POSITION;
	float3 color : COLOR0;
};

VertexShaderOutput vs(VertexShaderInput input) 
{
	VertexShaderOutput output;

	output.pos = mul(float4(input.pos.xyz, 1), Model);
	output.pos = mul(output.pos, ViewProjection);

	output.color = input.color;
	return output;
}

float4 ps(VertexShaderOutput input) : SV_TARGET 
{
	return float4(input.color, 1);
})_";

float cubeVertices[] =
{
	-1.0f, -1.0f, -1.0f,     0.0f, 0.0f, 0.0f,
	-1.0f, -1.0f,  1.0f,     0.0f, 0.0f, 1.0f,
	-1.0f,  1.0f, -1.0f,     0.0f, 1.0f, 0.0f,
	-1.0f,  1.0f,  1.0f,     0.0f, 1.0f, 1.0f,
	 1.0f, -1.0f, -1.0f,     1.0f, 0.0f, 0.0f,
	 1.0f, -1.0f,  1.0f,     1.0f, 0.0f, 1.0f,
	 1.0f,  1.0f, -1.0f,     1.0f, 1.0f, 0.0f,
	 1.0f,  1.0f,  1.0f,     1.0f, 1.0f, 1.0f,
};

uint16_t cubeIndices[] =
{
	2,1,0, // -x
	2,3,1,

	6,4,5, // +x
	6,5,7,

	0,1,5, // -y
	0,5,4,

	2,6,7, // +y
	2,7,3,

	0,4,6, // -z
	0,6,2,

	1,3,7, // +z
	1,7,5,
};


// Scene
vector<XrPosef> cubes(2, POSE_IDENTITY);

////////////////////////////////////////////////
// Graphics - Direct3D                             
////////////////////////////////////////////////

void D3DShutdown() 
{
	if (d3dContext) 
	{ 
		d3dContext->Release(); 
		d3dContext = nullptr; 
	}
	if (d3dDevice) 
	{ 
		d3dDevice->Release();  
		d3dDevice = nullptr; 
	}
}


void D3DDestroySwapchain(SwapchainInfo& swapchain) 
{
	for (uint32_t i = 0; i < swapchain.xrSwapchainImages.size(); i++) 
	{
		swapchain.depthStencilViews[i]->Release();
		swapchain.renderTargetViews[i]->Release();
	}
}


XMMATRIX D3DGetProjectionMatrix(XrFovf fov, float clip_near, float clip_far) 
{
	const float left = clip_near * tanf(fov.angleLeft);
	const float right = clip_near * tanf(fov.angleRight);
	const float down = clip_near * tanf(fov.angleDown);
	const float up = clip_near * tanf(fov.angleUp);

	return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
}


ID3DBlob* D3DCompileShader(const char* hlsl, const char* entrypoint, const char* target) {
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled, * errors;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
		printf("Error: D3DCompile failed %s", (char*)errors->GetBufferPointer());
	if (errors) errors->Release();

	return compiled;
}


void D3DInitializeResources()
{
	// Compile our shader code, and turn it into a shader resource!
	ID3DBlob* vertexShaderBytes = D3DCompileShader(shader, "vs", "vs_5_0");
	ID3DBlob* pixelShaderBytes = D3DCompileShader(shader, "ps", "ps_5_0");
	d3dDevice->CreateVertexShader(vertexShaderBytes->GetBufferPointer(), vertexShaderBytes->GetBufferSize(), nullptr, &vertexShader);
	d3dDevice->CreatePixelShader(pixelShaderBytes->GetBufferPointer(), pixelShaderBytes->GetBufferSize(), nullptr, &pixelShader);


	// CREATE INPUT LAYOUT                               
	// Describe how our mesh is laid out in memory
	D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
	{
		{"SV_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};

	d3dDevice->CreateInputLayout(vertexDesc, (UINT)_countof(vertexDesc), vertexShaderBytes->GetBufferPointer(), vertexShaderBytes->GetBufferSize(), &inputLayout);


	// CREATE GPU RESOURCES FROM VERTEX BUFFER, INDICES BUFFER, CONSTANT BUFFERS (NO DATA YET) // declared buffers on GPU, create by GPU and pass reference back  
	// its same as buffer b = new buffer, but gpu does creation and memory management?                           
	// Create GPU resources for our mesh's vertices and indices! Constant buffers are for passing transform
	// matrices into the shaders, so make a buffer for them too!
	D3D11_SUBRESOURCE_DATA vertexBufferData = { cubeVertices };
	D3D11_SUBRESOURCE_DATA indexBufferData = { cubeIndices };

	CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(cubeVertices), D3D11_BIND_VERTEX_BUFFER);
	CD3D11_BUFFER_DESC indexBufferDesc(sizeof(cubeIndices), D3D11_BIND_INDEX_BUFFER);

	CD3D11_BUFFER_DESC modelConstantBufferDesc(sizeof(ModelConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);
	CD3D11_BUFFER_DESC viewProjectionConstantBufferDesc(sizeof(ViewProjectionConstantBuffer), D3D11_BIND_CONSTANT_BUFFER);

	d3dDevice->CreateBuffer(&vertexBufferDesc, &vertexBufferData, &vertexBuffer);
	d3dDevice->CreateBuffer(&indexBufferDesc, &indexBufferData, &indexBuffer);
	d3dDevice->CreateBuffer(&modelConstantBufferDesc, nullptr, &modelConstantBuffer); // no data yet, constant buffer will  be updated every frame
	d3dDevice->CreateBuffer(&viewProjectionConstantBufferDesc, nullptr, &viewProjectionConstantBuffer);
}


////////////////////////////////////////////////
// OpenXR                             
////////////////////////////////////////////////

bool OpenXRInitialize()
{
	// Check if Direct3D 11 extension is available
	{
		renderingExtension = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
		bool renderingExtensionFound = false;

		uint32_t availableExtensionsCount = 0;
		xrEnumerateInstanceExtensionProperties(nullptr, 0, &availableExtensionsCount, nullptr);
		vector<XrExtensionProperties> xrAvailableExtensions(availableExtensionsCount, { XR_TYPE_EXTENSION_PROPERTIES });
		xrEnumerateInstanceExtensionProperties(nullptr, availableExtensionsCount, &availableExtensionsCount, xrAvailableExtensions.data());

		for (size_t i = 0; i < availableExtensionsCount; i++)
		{
			if (strcmp(renderingExtension, xrAvailableExtensions[i].extensionName) == 0)
			{
				renderingExtensionFound = true;
				break;
			}
		}

		if (!renderingExtensionFound)
		{
			return false;
		}
	}


	// Create XRInstance
	{
		XrInstanceCreateInfo xrInstanceCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
		xrInstanceCreateInfo.enabledExtensionCount = 1;
		xrInstanceCreateInfo.enabledExtensionNames = &renderingExtension;
		xrInstanceCreateInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
		strcpy_s(xrInstanceCreateInfo.applicationInfo.applicationName, "OpenXR Example");

		xrCreateInstance(&xrInstanceCreateInfo, &xrInstance);

		if (xrInstance == XR_NULL_HANDLE)
		{
			return false;
		}
	}


	// Get pointer to GetGraphicsRequirements function from extension
	{
		xrGetInstanceProcAddr(xrInstance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&ext_xrGetD3D11GraphicsRequirementsKHR));
	}


	// Create place hologram action set
	{
		XrActionSetCreateInfo actionSetInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy_s(actionSetInfo.actionSetName, "place_hologram_action_set");
		strcpy_s(actionSetInfo.localizedActionSetName, "Placement");
		xrCreateActionSet(xrInstance, &actionSetInfo, &xrActionSet);
	}


	// Get hand subaction paths
	{
		xrPath_HandSubactions[0] = StringToPath(xrInstance, "/user/hand/left");
		xrPath_HandSubactions[1] = StringToPath(xrInstance, "/user/hand/right");
	}


	// Create hand pose actions
	{
		XrActionCreateInfo handPoseAction_CreateInfo = { XR_TYPE_ACTION_CREATE_INFO };
		handPoseAction_CreateInfo.countSubactionPaths = _countof(xrPath_HandSubactions);
		handPoseAction_CreateInfo.subactionPaths = xrPath_HandSubactions;
		handPoseAction_CreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
		strcpy_s(handPoseAction_CreateInfo.actionName, "hand_pose");
		strcpy_s(handPoseAction_CreateInfo.localizedActionName, "Hand Pose");

		xrCreateAction(xrActionSet, &handPoseAction_CreateInfo, &xrAction_HandPose);
	}


	// Create hologram placement action
	{
		XrActionCreateInfo placeHologramAction_CreateInfo = { XR_TYPE_ACTION_CREATE_INFO };
		placeHologramAction_CreateInfo.countSubactionPaths = _countof(xrPath_HandSubactions);
		placeHologramAction_CreateInfo.subactionPaths = xrPath_HandSubactions;
		placeHologramAction_CreateInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		strcpy_s(placeHologramAction_CreateInfo.actionName, "place_hologram");
		strcpy_s(placeHologramAction_CreateInfo.localizedActionName, "Place Hologram");

		xrCreateAction(xrActionSet, &placeHologramAction_CreateInfo, &xrAction_Select);
	}


	// Suggest default bindings between actions and the khronos simple controller interaction profile
	{
		XrActionSuggestedBinding suggestedBindings[] =
		{
			{ xrAction_HandPose, StringToPath(xrInstance, "/user/hand/left/input/grip/pose") },
			{ xrAction_HandPose, StringToPath(xrInstance, "/user/hand/right/input/grip/pose") },
			{ xrAction_Select, StringToPath(xrInstance, "/user/hand/left/input/select/click") },
			{ xrAction_Select, StringToPath(xrInstance, "/user/hand/right/input/select/click")}
		};

		XrInteractionProfileSuggestedBinding interactionProfileSuggestedBindings = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		interactionProfileSuggestedBindings.interactionProfile = StringToPath(xrInstance, "/interaction_profiles/khr/simple_controller");
		interactionProfileSuggestedBindings.suggestedBindings = &suggestedBindings[0];
		interactionProfileSuggestedBindings.countSuggestedBindings = _countof(suggestedBindings);
		xrSuggestInteractionProfileBindings(xrInstance, &interactionProfileSuggestedBindings);
	}


	// Get System ID for head mounted display form factor
	{
		XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		xrGetSystem(xrInstance, &systemInfo, &xrSystemId);
	}


	// Choose environment blend mode valid for the device
	{
		uint32_t availableBlendModesCount = 0;
		xrEnumerateEnvironmentBlendModes(xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1, &availableBlendModesCount, nullptr);
		vector<XrEnvironmentBlendMode> xrEnvironmentBlendModes(availableBlendModesCount);

		xrEnumerateEnvironmentBlendModes(xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, availableBlendModesCount, &availableBlendModesCount, xrEnvironmentBlendModes.data());
		xrEnvironmentBlendMode = xrEnvironmentBlendModes[0];
	}


	// Get D3D11 Graphics requirements for the app
	{
		ext_xrGetD3D11GraphicsRequirementsKHR(xrInstance, xrSystemId, &xrGraphicsRequirements);
	}


	// Create DXGI Factory
	{
		CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&dxgiFactory));
	}


	// Use DXGI Factory to try finding graphics card with matching luid from graphics requirements
	{
		int adapterIndex = 0;
		bool foundAdapter = false;
		while (dxgiFactory->EnumAdapters1(adapterIndex++, &graphicsAdapter) == S_OK) 
		{
			graphicsAdapter->GetDesc1(&adapterDesc);

			if (memcmp(&adapterDesc.AdapterLuid, &xrGraphicsRequirements.adapterLuid, sizeof(&xrGraphicsRequirements.adapterLuid)) == 0) {
				foundAdapter = true;
				break;
			}

		}
		dxgiFactory->Release();

		if (!foundAdapter)
		{
			return false;
		}
	}


	// Create D3D11 device for required feature levels
	{
		if (FAILED(D3D11CreateDevice(graphicsAdapter, D3D_DRIVER_TYPE_UNKNOWN, 0, 0, featureLevels, _countof(featureLevels), D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext)))
		{
			return false;
		}

		graphicsAdapter->Release();
	}


	// Create XRSession with graphics device and system ID
	{
		XrGraphicsBindingD3D11KHR xrGraphicsBinding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
		xrGraphicsBinding.device = d3dDevice;

		XrSessionCreateInfo xrCreateSessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
		xrCreateSessionInfo.next = &xrGraphicsBinding;
		xrCreateSessionInfo.systemId = xrSystemId;
		xrCreateSession(xrInstance, &xrCreateSessionInfo, &xrSession);

		if (xrSession == XR_NULL_HANDLE)
		{
			return false;
		}
	}


	// Attach the action set to the session
	{
		XrSessionActionSetsAttachInfo actionSetsAttachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		actionSetsAttachInfo.countActionSets = 1;
		actionSetsAttachInfo.actionSets = &xrActionSet;
		xrAttachSessionActionSets(xrSession, &actionSetsAttachInfo);
	}


	// Create reference space for positioning holograms
	{
		XrReferenceSpaceCreateInfo xrReferenceSpaceCreateInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		xrReferenceSpaceCreateInfo.poseInReferenceSpace = POSE_IDENTITY;
		xrReferenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		xrCreateReferenceSpace(xrSession, &xrReferenceSpaceCreateInfo, &xrSpace);
	}


	// Create spaces for each hand pose	
	for (int32_t i = 0; i < 2; i++) 
	{
		XrActionSpaceCreateInfo xrActionSpaceCreateInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		xrActionSpaceCreateInfo.action = xrAction_HandPose;
		xrActionSpaceCreateInfo.poseInActionSpace = POSE_IDENTITY;
		xrActionSpaceCreateInfo.subactionPath = xrPath_HandSubactions[i];
		xrCreateActionSpace(xrSession, &xrActionSpaceCreateInfo, &xrSpace_Hands[i]);
	}


	// Enumerate device viewpoints and populate ViewConfigurationViews
	{
		xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
		xrViewConfigurationViews.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
		xrViews.resize(viewCount, { XR_TYPE_VIEW });
		xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, xrViewConfigurationViews.data());
	}


	// Create a swapchain for every viewpoint
	for (uint32_t i = 0; i < viewCount; i++) 
	{

		XrSwapchain xrSwapChain;
		XrSwapchainCreateInfo xrSwapchainCreateInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		SwapchainInfo swapchainInfo = {};
		uint32_t swapchainLength = 0;


		// Use info from viewpoint view configuration view to create swapchain
		{
			XrViewConfigurationView& xrViewConfigurationView = xrViewConfigurationViews[i];
			xrSwapchainCreateInfo.arraySize = 1;
			xrSwapchainCreateInfo.mipCount = 1;
			xrSwapchainCreateInfo.faceCount = 1;
			xrSwapchainCreateInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			xrSwapchainCreateInfo.width = xrViewConfigurationView.recommendedImageRectWidth;
			xrSwapchainCreateInfo.height = xrViewConfigurationView.recommendedImageRectHeight;
			xrSwapchainCreateInfo.sampleCount = xrViewConfigurationView.recommendedSwapchainSampleCount;
			xrSwapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			
			xrCreateSwapchain(xrSession, &xrSwapchainCreateInfo, &xrSwapChain);
		}


		// Find out how many textures were generated for the swapchain by device runtime
		{
			xrEnumerateSwapchainImages(xrSwapChain, 0, &swapchainLength, nullptr);
		}


		// Cache created swapchain handle, dimension, images and views, so we can draw onto it later
		{
			swapchainInfo.width = xrSwapchainCreateInfo.width;
			swapchainInfo.height = xrSwapchainCreateInfo.height;
			swapchainInfo.xrSwapchainHandle = xrSwapChain;
			swapchainInfo.xrSwapchainImages.resize(swapchainLength, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
			swapchainInfo.depthStencilViews.resize(swapchainLength);
			swapchainInfo.renderTargetViews.resize(swapchainLength);
		}


		// Cache swapchain images created by runtime device
		{
			xrEnumerateSwapchainImages(swapchainInfo.xrSwapchainHandle, swapchainLength, &swapchainLength, (XrSwapchainImageBaseHeader*)swapchainInfo.xrSwapchainImages.data());
		}


		// Create render target view and depth stencial view for every swapchain image
		for (uint32_t i = 0; i < swapchainLength; i++)
		{
			D3D11_TEXTURE2D_DESC colorTextureDesc;
			ID3D11Texture2D* depthTexture;


			// Create render target view resource for swapchain image
			{			
				swapchainInfo.xrSwapchainImages[i].texture->GetDesc(&colorTextureDesc);

				D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc = {};
				renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				renderTargetViewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				d3dDevice->CreateRenderTargetView(swapchainInfo.xrSwapchainImages[i].texture, &renderTargetViewDesc, &swapchainInfo.renderTargetViews[i]);
			}
			
			
			// Create texture for depth stencil
			{
				D3D11_TEXTURE2D_DESC depthTextureDesc = {};
				depthTextureDesc.SampleDesc.Count = 1;
				depthTextureDesc.MipLevels = 1;
				depthTextureDesc.Width = colorTextureDesc.Width;
				depthTextureDesc.Height = colorTextureDesc.Height;
				depthTextureDesc.ArraySize = colorTextureDesc.ArraySize;
				depthTextureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
				depthTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
				
				d3dDevice->CreateTexture2D(&depthTextureDesc, nullptr, &depthTexture);
			}


			// Create depth stencil view resource for swapchain image
			{
				D3D11_DEPTH_STENCIL_VIEW_DESC dephViewDesc = {};
				dephViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				dephViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
				d3dDevice->CreateDepthStencilView(depthTexture, &dephViewDesc, &swapchainInfo.depthStencilViews[i]);
			}


			// We don't need direct access to the ID3D11Texture2D object anymore, we only need the view
			depthTexture->Release();
		}

		SwapchainsInfo.push_back(swapchainInfo);
	}

	return true;
}



void OpenXRProcessEvents(bool& exit) 
{
	XrEventDataBuffer eventData = { XR_TYPE_EVENT_DATA_BUFFER };

	// Process all OpenXR events
	while (xrPollEvent(xrInstance, &eventData) == XR_SUCCESS) 
	{
		// We are mainly interested in Session state changes
		switch (eventData.type) 
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: // Session state change is where we can begin and end sessions, as well as find quit messages!
		{
			XrEventDataSessionStateChanged* stateChangedEventData = (XrEventDataSessionStateChanged*)&eventData;
			xrSessionState = stateChangedEventData->state;

			switch (xrSessionState) 
			{

			case XR_SESSION_STATE_READY: // Ready to enable action polling, scene update and frame rendering in main loop
			{
				XrSessionBeginInfo xrSessionBeginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
				xrSessionBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xrBeginSession(xrSession, &xrSessionBeginInfo);
				IsXrSessionRunning = true;
			} break;

			case XR_SESSION_STATE_STOPPING: {
				IsXrSessionRunning = false;
				xrEndSession(xrSession);
			} break;

			case XR_SESSION_STATE_EXITING: // Exit main loop and quit if session exiting     
				exit = true;              
				return;

			case XR_SESSION_STATE_LOSS_PENDING: // Exit main loop and quit if session lost
				exit = true;              
				return;
			}
		} break;

		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: // Exit main loop if Instace lost
			exit = true; 
			return;
		}

		eventData = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}


void OpenXRPollActions() 
{ 
	// Actions only processed if session focused
	{
		if (xrSessionState != XR_SESSION_STATE_FOCUSED)
		{
			return;
		}
	}

	// Sync actions with up-to-date input data
	{
		XrActiveActionSet xrActiveActionSet = { };
		xrActiveActionSet.actionSet = xrActionSet;
		xrActiveActionSet.subactionPath = XR_NULL_PATH;

		XrActionsSyncInfo xrActionsSyncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
		xrActionsSyncInfo.countActiveActionSets = 1;
		xrActionsSyncInfo.activeActionSets = &xrActiveActionSet;

		xrSyncActions(xrSession, &xrActionsSyncInfo);
	}


	// Cache actions state for each hand
	for (uint32_t handIndex = 0; handIndex < 2; handIndex++) 
	{
		XrActionStateGetInfo actionInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		actionInfo.subactionPath = xrPath_HandSubactions[handIndex];
		XrActionStatePose handPoseState = { XR_TYPE_ACTION_STATE_POSE };
		XrActionStateBoolean handSelectState = { XR_TYPE_ACTION_STATE_BOOLEAN };


		// Get up to date hand pose state
		{
			actionInfo.action = xrAction_HandPose;
			xrGetActionStatePose(xrSession, &actionInfo, &handPoseState);

			xrBool_IsHandPoseActive[handIndex] = handPoseState.isActive; // this is only to get pose active, pose only comes after frame time predicted. we dont know where hand will be 
		}


		// Get up to date state of select action
		{
			actionInfo.action = xrAction_Select;
			xrGetActionStateBoolean(xrSession, &actionInfo, &handSelectState);
		}


		// Add new cube to the scene if new select action detected
		{
			if (handSelectState.currentState && handSelectState.changedSinceLastSync)
			{
				XrSpaceLocation handSpaceLocation = { XR_TYPE_SPACE_LOCATION };
				if (XR_UNQUALIFIED_SUCCESS(xrLocateSpace(xrSpace_Hands[handIndex], xrSpace, handSelectState.lastChangeTime, &handSpaceLocation)) &&
					(handSpaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
					(handSpaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
				{
					cubes.push_back(handSpaceLocation.pose); // add hand pose in the past to cube, as this happened in the past, we know where hand was
				}
			}
		}
	}
}


void OpenXRRenderFrame()
{
	XrFrameState frameState = { XR_TYPE_FRAME_STATE };


	// Wait for previous frame finished displaying and a prediction of when the next frame will be displayed, used for pose prediction
	{
		xrWaitFrame(xrSession, nullptr, &frameState);
	}


	// Sinalize we are about to start rendering. This can return some interesting flags like XR_SESSION_VISIBILITY_UNAVAILABLE
	{
		xrBeginFrame(xrSession, nullptr);
	}


	// Use predicted display time to update cube poses to follow hands if session has focus and can receive user input
	{
		if (xrSessionState == XR_SESSION_STATE_FOCUSED)
		{
			for (size_t handIndex = 0; handIndex < 2; handIndex++)
			{
				if (!xrBool_IsHandPoseActive[handIndex])
				{
					continue;
				}


				// Get predicted hand pose by locating hand space on predicted time for acurate location and reduced perceived lag
				{
					XrSpaceLocation handSpaceLocation = { XR_TYPE_SPACE_LOCATION };
					if (XR_UNQUALIFIED_SUCCESS(xrLocateSpace(xrSpace_Hands[handIndex], xrSpace, frameState.predictedDisplayTime, &handSpaceLocation)) &&
						(handSpaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
						(handSpaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
					{
						xrPosef_Hands[handIndex] = handSpaceLocation.pose;
					}
				}

			}


			// Update the predicted poses of the cubes attached to the hands to match predicted hand poses
			for (size_t handIndex = 0; handIndex < 2; handIndex++)
			{
				cubes[handIndex] = xrBool_IsHandPoseActive[handIndex] ? xrPosef_Hands[handIndex] : POSE_IDENTITY;
			}
		}
	}


	XrCompositionLayerBaseHeader* layer = nullptr;
	XrCompositionLayerProjection layerProjection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	vector<XrCompositionLayerProjectionView> layerProjectionViews;


	// Lets render our views if session visible
	if (xrSessionState == XR_SESSION_STATE_VISIBLE || xrSessionState == XR_SESSION_STATE_FOCUSED)
	{
		// Locate each viewpoint at the predicted time
		{
			uint32_t viewCount;
			XrViewState viewState = { XR_TYPE_VIEW_STATE };
			XrViewLocateInfo viewLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
			viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			viewLocateInfo.displayTime = frameState.predictedDisplayTime;
			viewLocateInfo.space = xrSpace;

			xrLocateViews(xrSession, &viewLocateInfo, &viewState, (uint32_t)xrViews.size(), &viewCount, xrViews.data());
			layerProjectionViews.resize(viewCount);
		}


		// Render views from each viewpoint
		for (uint32_t i = 0; i < viewCount; i++) 
		{
			uint32_t imageId;

			// Ask runtime which swapchain image is next for rendering 
			{
				XrSwapchainImageAcquireInfo imageAcquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				xrAcquireSwapchainImage(SwapchainsInfo[i].xrSwapchainHandle, &imageAcquireInfo, &imageId);
			}


			// Wait until the image is ready to render to
			{
				XrSwapchainImageWaitInfo imageWaitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				imageWaitInfo.timeout = XR_INFINITE_DURATION;
				xrWaitSwapchainImage(SwapchainsInfo[i].xrSwapchainHandle, &imageWaitInfo);
			}


			// Set up viewpoint rendering information
			{
				layerProjectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
				layerProjectionViews[i].pose = xrViews[i].pose;
				layerProjectionViews[i].fov = xrViews[i].fov;
				layerProjectionViews[i].subImage.swapchain = SwapchainsInfo[i].xrSwapchainHandle;
				layerProjectionViews[i].subImage.imageRect.offset = { 0, 0 };
				layerProjectionViews[i].subImage.imageRect.extent = { SwapchainsInfo[i].width, SwapchainsInfo[i].height };
			}

			
			// Set D3D viewport we will render onto with same swapchain image dimension
			{
				XrRect2Di& rect = layerProjectionViews[i].subImage.imageRect;
				D3D11_VIEWPORT viewport = CD3D11_VIEWPORT((float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height);
				d3dContext->RSSetViewports(1, &viewport);
			}


			// Clear swapchain color and depth views, and set them up for rendering on d3D rendering pipeline
			{
				float clear[] = { 0, 0, 0, 1 };
				d3dContext->ClearRenderTargetView(SwapchainsInfo[i].renderTargetViews[imageId], clear);
				d3dContext->ClearDepthStencilView(SwapchainsInfo[i].depthStencilViews[imageId], D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
				d3dContext->OMSetRenderTargets(1, &SwapchainsInfo[i].renderTargetViews[imageId], SwapchainsInfo[i].depthStencilViews[imageId]);
			}

                                
			// Set the active shaders and constant buffers on Vector and Pixel Shader stages of D3D rendering pipeline
			{
				ID3D11Buffer* const constantBuffers[] = { modelConstantBuffer , viewProjectionConstantBuffer };
				d3dContext->VSSetConstantBuffers(0, (UINT)std::size(constantBuffers), constantBuffers);
				d3dContext->VSSetShader(vertexShader, nullptr, 0);
				d3dContext->PSSetShader(pixelShader, nullptr, 0);
			}


			//	Hook cube mesh triangles data, the vertex buffer and index buffer on Input Assembly stage of D3D rendering pipeline
			{
				UINT strides[] = { sizeof(float) * 6 };
				UINT offsets[] = { 0 };
				d3dContext->IASetVertexBuffers(0, 1, &vertexBuffer, strides, offsets);
				d3dContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
				d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				d3dContext->IASetInputLayout(inputLayout);
			}

                               
			// Set up view projection matrix based on predicted camera pose information and update shader's view projection constant buffer
			{
				XMMATRIX ProjectionMatrix = D3DGetProjectionMatrix(layerProjectionViews[i].fov, 0.05f, 100.0f);
				XMMATRIX ViewMatrix = XMMatrixInverse(nullptr,
					XMMatrixAffineTransformation
					(
						DirectX::g_XMOne,
						DirectX::g_XMZero,
						XMLoadFloat4((XMFLOAT4*)&layerProjectionViews[i].pose.orientation),
						XMLoadFloat3((XMFLOAT3*)&layerProjectionViews[i].pose.position)
					));


				ViewProjectionConstantBuffer viewproj;
				XMStoreFloat4x4(&viewproj.ViewProjection, XMMatrixTranspose(ViewMatrix * ProjectionMatrix));
				d3dContext->UpdateSubresource(viewProjectionConstantBuffer, 0, nullptr, &viewproj, 0, 0);
			}


			// Set up model transform matrix for every cube in the stack with updated cube pose, update shader's model contant buffer and draw cube triangles                  
			{
				ModelConstantBuffer model;

				for (size_t i = 0; i < cubes.size(); i++)
				{
					XMMATRIX ModelMatrix = XMMatrixAffineTransformation(
						DirectX::g_XMOne * 0.05f, DirectX::g_XMZero,
						XMLoadFloat4((XMFLOAT4*)&cubes[i].orientation),
						XMLoadFloat3((XMFLOAT3*)&cubes[i].position));


					XMStoreFloat4x4(&model.Model, XMMatrixTranspose(ModelMatrix));
					d3dContext->UpdateSubresource(modelConstantBuffer, 0, nullptr, &model, 0, 0);
					d3dContext->DrawIndexed((UINT)_countof(cubeIndices), 0, 0);
				}
			}


			// Tell runtime we are finished with rendering to this swapchain image
			XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(SwapchainsInfo[i].xrSwapchainHandle, &release_info);
		}


		// Add rendered views to the final layer that will be passed to Open XR Runtime 
		{
			layerProjection.space = xrSpace;
			layerProjection.viewCount = (uint32_t)layerProjectionViews.size();
			layerProjection.views = layerProjectionViews.data();


			layer = (XrCompositionLayerBaseHeader*)&layerProjection;
		}
	}


	// Send rendered layer for display and end frame work
	{
		XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
		end_info.displayTime = frameState.predictedDisplayTime;
		end_info.environmentBlendMode = xrEnvironmentBlendMode;
		end_info.layerCount = layer == nullptr ? 0 : 1;
		end_info.layers = &layer;
		xrEndFrame(xrSession, &end_info);
	}
}


void OpenXRShutdown() 
{
	// We used a graphics API to initialize the swapchain data, so we'll
	// give it a chance to release anythig here!
	for (int32_t i = 0; i < SwapchainsInfo.size(); i++) 
	{
		xrDestroySwapchain(SwapchainsInfo[i].xrSwapchainHandle);
		D3DDestroySwapchain(SwapchainsInfo[i]);
	}

	SwapchainsInfo.clear();

	// Release all the other OpenXR resources that we've created!
	// What gets allocated, must get deallocated!
	if (xrActionSet != XR_NULL_HANDLE) 
	{
		if (xrSpace_Hands[0] != XR_NULL_HANDLE) xrDestroySpace(xrSpace_Hands[0]);
		if (xrSpace_Hands[1] != XR_NULL_HANDLE) xrDestroySpace(xrSpace_Hands[1]);
		xrDestroyActionSet(xrActionSet);
	}

	if (xrSpace != XR_NULL_HANDLE) xrDestroySpace(xrSpace);
	if (xrSession != XR_NULL_HANDLE) xrDestroySession(xrSession);
	if (xrInstance != XR_NULL_HANDLE) xrDestroyInstance(xrInstance);
}


                       
int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	if (!OpenXRInitialize()) 
	{
		D3DShutdown();
		return 1;
	}

	D3DInitializeResources();

	bool exit = false;
	while (!exit) 
	{
		OpenXRProcessEvents(exit);

		if (IsXrSessionRunning)
		{
			OpenXRPollActions();
			OpenXRRenderFrame();
		}
		else
		{
			// Throttle loop when wait frame is not called
			this_thread::sleep_for(chrono::milliseconds(250));
		}
	}

	OpenXRShutdown();
	D3DShutdown();
	return 0;
}
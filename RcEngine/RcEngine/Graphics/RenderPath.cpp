#include <Graphics/RenderPath.h>
#include <Graphics/RenderDevice.h>
#include <Graphics/GraphicsResource.h>
#include <Graphics/VertexDeclaration.h>
#include <Graphics/FrameBuffer.h>
#include <Graphics/Material.h>
#include <Graphics/Effect.h>
#include <Graphics/Camera.h>
#include <Graphics/Geometry.h>
#include <Graphics/RenderQueue.h>
#include <Graphics/RenderFactory.h>
#include <Graphics/RenderOperation.h>
#include <Graphics/CascadedShadowMap.h>
#include <Graphics/DebugDrawManager.h>
#include <Graphics/AmbientOcclusion.h>
#include <MainApp/Application.h>
#include <MainApp/Window.h>
#include <Scene/SceneManager.h>
#include <Scene/SceneNode.h>
#include <Scene/Light.h>
#include <Core/Environment.h>
#include <Core/Exception.h>
#include <Math/MathUtil.h>
#include <Resource/ResourceManager.h>
#include <Input/InputSystem.h>

namespace RcEngine {

RenderPath::RenderPath()
{
	mDevice = Environment::GetSingleton().GetRenderDevice();
	mSceneMan = Environment::GetSingleton().GetSceneManager();
	if (mDevice == NULL || mSceneMan == NULL)
	{
		ENGINE_EXCEPT(Exception::ERR_INTERNAL_ERROR, "Create device and SceneManager first!", "Renderer::Init");
		return;
	}
}

void RenderPath::OnGraphicsInit( const shared_ptr<Camera>& camera )
{
	mCamera = camera;
	BuildFullscreenTriangle(mFullscreenTrangle);
}

void RenderPath::DrawOverlays()
{
	//// Update overlays
	//UIManager& uiMan = UIManager::GetSingleton();

	//uiMan.Render();
	//mSceneMan->UpdateOverlayQueue();

	//RenderBucket& guiBucket =mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketOverlay);	
	//for (const RenderQueueItem& renderItem : guiBucket) 
	//	renderItem.Renderable->Render();
}

//----------------------------------------------------------------------------------------------
ForwardPath::ForwardPath()
	: RenderPath(),
	  mShadowMan(NULL)
{

}

ForwardPath::~ForwardPath()
{
	SAFE_DELETE(mShadowMan);
}

void ForwardPath::OnGraphicsInit( const shared_ptr<Camera>& camera )
{
	RenderPath::OnGraphicsInit(camera);
	mShadowMan = new CascadedShadowMap(mDevice);
}

void ForwardPath::OnWindowResize( uint32_t width, uint32_t height )
{

}

void ForwardPath::RenderScene()
{
	shared_ptr<FrameBuffer> screenFB = mDevice->GetCurrentFrameBuffer();
	shared_ptr<Camera> viewCamera = screenFB->GetCamera();

	mDevice->BindFrameBuffer(screenFB);
	screenFB->Clear(CF_Color | CF_Depth, ColorRGBA::ColorRGBA(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, 0);

	mSceneMan->UpdateRenderQueue(viewCamera, RO_None, RenderQueue::BucketAll, 0);

	// Draw Sky box first
	const RenderBucket& bkgBucket = mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketBackground, false);
	for (const RenderQueueItem& item : bkgBucket)
		item.Renderable->Render();

	// Update Light Queue
	mSceneMan->UpdateLightQueue(*viewCamera);
	const LightQueue& sceneLights = mSceneMan->GetLightQueue();

	// Draw opaque 
	RenderBucket opaqueBucket;
	mSceneMan->GetRenderQueue().SwapRenderBucket(opaqueBucket, RenderQueue::BucketOpaque);
	
	for (Light* light : sceneLights)
	{
		bool bCastShadow = light->GetCastShadow();

		String techName;
		if (light->GetLightType() == LT_DirectionalLight)
		{
			if (bCastShadow)
				mShadowMan->MakeCascadedShadowMap(*light);

			techName = "DirectionalLighting";

			for (const RenderQueueItem& renderItem : opaqueBucket) 
			{
				auto material = renderItem.Renderable->GetMaterial();

				material->SetCurrentTechnique(techName);
				
				shared_ptr<Effect> effect = material->GetEffect();

				float3 lightColor = light->GetLightColor() * light->GetLightIntensity();
				effect->GetParameterByUsage(EPU_Light_Color)->SetValue(lightColor);

				const float3& worldDirection = light->GetDerivedDirection();
				float4 lightDir(worldDirection[0], worldDirection[1], worldDirection[2], 0.0f);
				effect->GetParameterByUsage(EPU_Light_Dir)->SetValue(lightDir);

				EffectParameter* enableShadowParam = effect->GetParameterByName("ShadowEnabled");
				enableShadowParam->SetValue(bCastShadow);
				if (bCastShadow)
				{    
					effect->GetConstantBuffer("cbPossionDiskSamples")->SetBuffer(mShadowMan->mPossionSamplesCBuffer);
					
					effect->GetParameterByName("CascadeShadowMap")->SetValue( mShadowMan->mShadowTexture->GetShaderResourceView());
					effect->GetParameterByName("LightView")->SetValue(mShadowMan->mLightViewMatrix);
					effect->GetParameterByName("NumCascades")->SetValue((int)light->GetShadowCascades());
					effect->GetParameterByName("BorderPaddingMinMax")->SetValue(mShadowMan->mBorderPaddingMinMax);
					effect->GetParameterByName("CascadeScale")->SetValue(&mShadowMan->mShadowCascadeScale[0], MAX_CASCADES);
					effect->GetParameterByName("CascadeOffset")->SetValue(&mShadowMan->mShadowCascadeOffset[0], MAX_CASCADES); 
					effect->GetParameterByName("InvShadowMapSize")->SetValue(1.0f / SHADOW_MAP_SIZE);
					//effect->GetParameterByName("CascadeBlendArea")->SetValue(mShadowMan->mCascadeBlendArea);
				}

				renderItem.Renderable->Render();
			}
		}
	}
}



//--------------------------------------------------------------------------------------------
DeferredPath::DeferredPath()
	: RenderPath(),
	  mVisualLights(false),
	  mVisualLightsWireframe(false),
	  mShadowMan(NULL),
	  mAmbientOcclusion(NULL)
{

}

DeferredPath::~DeferredPath()
{
	SAFE_DELETE(mShadowMan);
	SAFE_DELETE(mAmbientOcclusion);
}

void DeferredPath::OnGraphicsInit( const shared_ptr<Camera>& camera )
{
	RenderPath::OnGraphicsInit(camera);

	Window* appWindow = Application::msApp->GetMainWindow();
	const uint32_t windowWidth = appWindow->GetWidth();
	const uint32_t windowHeight = appWindow->GetHeight();

	ResourceManager& resMan = ResourceManager::GetSingleton();

	// Load deferred lighting effect
	mDeferredEffect = resMan.GetResourceByName<Effect>(RT_Effect, "DeferredLighting.effect.xml", "General");
	mToneMapEffect = resMan.GetResourceByName<Effect>(RT_Effect, "HDR.effect.xml", "General");
	mDebugEffect = resMan.GetResourceByName<Effect>(RT_Effect, "DebugView.effect.xml", "General");

	mDirLightTech = mDeferredEffect->GetTechniqueByName("DirectionalLighting");
	mPointLightTech = mDeferredEffect->GetTechniqueByName("PointLighting");
	mSpotLightTech = mDeferredEffect->GetTechniqueByName("SpotLighting");
	mShadingTech = mDeferredEffect->GetTechniqueByName("Shading");

	CreateBuffers(windowWidth, windowHeight);
	
	// Build light volume
	BuildConeOperation(mSpotLightShape, 1.0f, 1.0f, 30);
	BuildShpereOperation(mPointLightShape, 1.0f, 30, 30);

	mGBufferFB->SetCamera(camera);
	mLightAccumulateFB->SetCamera(camera);
	mHDRFB->SetCamera(camera);

	//-------------------------
	//mAmbientOcclusion = new AmbientOcclusion(mDevice, SSAO_HBAO, windowWidth, windowHeight);
	mShadowMan = new CascadedShadowMap(mDevice);
}

void DeferredPath::CreateBuffers( uint32_t windowWidth, uint32_t windowHeight )
{
	RenderFactory* factory = mDevice->GetRenderFactory();

#ifdef _DEBUG
	uint32_t acessHint = EAH_GPU_Write | EAH_GPU_Read;
#else
	uint32_t acessHint = EAH_GPU_Write | EAH_GPU_Read;
#endif

	uint32_t rtCreateFlag = TexCreate_ShaderResource | TexCreate_RenderTarget;
	uint32_t dsCreateFlag = TexCreate_ShaderResource | TexCreate_DepthStencilTarget;

	// Init GBuffer
	if (!mGBufferFB)
	{
		mGBufferFB = factory->CreateFrameBuffer(windowWidth, windowHeight);
	}
	else
	{
		mDevice->BindFrameBuffer(mGBufferFB);
		mGBufferFB->DetachAll();
		mGBufferFB->Resize(windowWidth, windowHeight);
	}
	mGBufferFB->SetViewport(Viewport(0.0f, 0.f, float(windowWidth), float(windowHeight)));

	mGBuffer[0] = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA16F, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mGBuffer[1] = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA8_UNORM, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mDepthStencilBuffer = factory->CreateTexture2D(windowWidth, windowHeight, PF_D24S8, 1, 1, 1, 0, acessHint, dsCreateFlag, NULL);

	mGBufferRTV[0] = factory->CreateRenderTargetView2D(mGBuffer[0], 0, 0);
	mGBufferRTV[1] = factory->CreateRenderTargetView2D(mGBuffer[1], 0, 0);
	mDepthStencilView = factory->CreateDepthStencilView(mDepthStencilBuffer, 0, 0);

	mGBufferFB->AttachRTV(ATT_DepthStencil, mDepthStencilView);
	mGBufferFB->AttachRTV(ATT_Color0, mGBufferRTV[0]);
	mGBufferFB->AttachRTV(ATT_Color1, mGBufferRTV[1]);

	// Init light buffer
	if (!mLightAccumulateFB)
	{
		mLightAccumulateFB = factory->CreateFrameBuffer(windowWidth, windowHeight);
	}
	else
	{
		mDevice->BindFrameBuffer(mLightAccumulateFB);
		mLightAccumulateFB->DetachAll();
		mLightAccumulateFB->Resize(windowWidth, windowHeight);
	}
	mLightAccumulateFB->SetViewport(Viewport(0.0f, 0.f, float(windowWidth), float(windowHeight)));

	mLightAccumulateBuffer = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA16F, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mDepthStencilBufferLight = factory->CreateTexture2D(windowWidth, windowHeight, PF_D24S8, 1, 1, 1, 0, acessHint, dsCreateFlag, NULL);

	mLightAccumulateRTV = factory->CreateRenderTargetView2D(mLightAccumulateBuffer, 0, 0);
	mDepthStencilBufferLightView = factory->CreateDepthStencilView(mDepthStencilBufferLight, 0, 0);

	mLightAccumulateFB->AttachRTV(ATT_DepthStencil, mDepthStencilBufferLightView);
	mLightAccumulateFB->AttachRTV(ATT_Color0, mLightAccumulateRTV);

	// HDR buffer
	if (!mHDRFB)
	{
		mHDRFB = factory->CreateFrameBuffer(windowWidth, windowHeight);
	}
	else
	{
		mDevice->BindFrameBuffer(mHDRFB);
		mHDRFB->DetachAll();
		mHDRFB->Resize(windowWidth, windowHeight);
	}
	mHDRFB->SetViewport(Viewport(0.0f, 0.f, float(windowWidth), float(windowHeight)));

	mHDRBuffer = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA16F, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mHDRBufferRTV = factory->CreateRenderTargetView2D(mHDRBuffer, 0, 0);
	mHDRFB->AttachRTV(ATT_Color0, mHDRBufferRTV);

	uint32_t dsvReadOnly = DSVCreate_ReadOnly_Stencil | DSVCreate_ReadOnly_Depth;
	mDepthStencilViewReadOnly = factory->CreateDepthStencilView(mDepthStencilBuffer, 0, 0, dsvReadOnly);
	mHDRFB->AttachRTV(ATT_DepthStencil, mDepthStencilViewReadOnly);

	// bind shader input
	mDeferredEffect->GetParameterByName("GBuffer0")->SetValue(mGBuffer[0]->GetShaderResourceView());
	mDeferredEffect->GetParameterByName("GBuffer1")->SetValue(mGBuffer[1]->GetShaderResourceView());
	mDeferredEffect->GetParameterByName("DepthBuffer")->SetValue(mDepthStencilBuffer->GetShaderResourceView());
	mDeferredEffect->GetParameterByName("LightAccumulateBuffer")->SetValue(mLightAccumulateBuffer->GetShaderResourceView());	

	mToneMapEffect->GetParameterByName("HDRBuffer")->SetValue(mHDRBuffer->GetShaderResourceView());	
}

void DeferredPath::OnWindowResize( uint32_t windowWidth, uint32_t windowHeight )
{
	CreateBuffers(windowWidth, windowHeight);
}

void DeferredPath::RenderScene()
{
	GenereateGBuffer();

	/*if (mAmbientOcclusion)
	{
		mAmbientOcclusion->Apply(*mCamera, mDepthStencilBuffer);
		mAmbientOcclusion->Visualize(mAmbientOcclusion->GetAmbientOcclusionSRV());
		return;
	}*/

	DeferredLighting();
	DeferredShading();

	// Draw Light
	if (mVisualLightsWireframe)
	{
		EffectTechnique* debugTech = mDebugEffect->GetTechniqueByName("DebugShape");

		mDebugEffect->GetParameterByName("ViewProj")->SetValue(mCamera->GetEngineViewProjMatrix());

		for (Light* light : mSceneMan->GetSceneLights())
		{
			const float3& lightColor = light->GetLightColor();
			mDebugEffect->GetParameterByName("Color")->SetValue(lightColor);

			if (light->GetLightType() == LT_PointLight)
			{
				float radius = light->GetRange(); 
				float4x4 world = CreateScaling(radius, radius, radius) * CreateTranslation(light->GetDerivedPosition());

				mDebugEffect->GetParameterByName("World")->SetValue(world);
				mDevice->Draw(debugTech, mPointLightShape);
			}
			else if (light->GetLightType() == LT_SpotLight) 
			{
				float spotInnerAngle = light->GetSpotInnerAngle();
				float spotOuterAngle = light->GetSpotOuterAngle();

				float scaleHeight = light->GetRange();
				float scaleBase = scaleHeight * tanf(spotOuterAngle * 0.5f);

				const float3& worldPos = light->GetDerivedPosition();
				const float3& worldDir = light->GetDerivedDirection();
				float3 rotAxis = Cross(float3(0, 1, 0), worldDir);
				//float rotAngle = acosf(Dot(float3(0, 1, 0), worldDirection));
				float4x4 rotation = CreateRotationAxis(rotAxis, acosf(worldDir.Y()));

				float4x4 world = CreateScaling(scaleBase, scaleHeight, scaleBase) * rotation *
								 CreateTranslation(worldPos);
				
				mDebugEffect->GetParameterByName("World")->SetValue(world);
				mDevice->Draw(debugTech, mSpotLightShape);
			}
		}
	}

	PostProcess();
}

void DeferredPath::GenereateGBuffer()
{
	mDevice->BindFrameBuffer(mGBufferFB);
	mGBufferFB->Clear(CF_Color | CF_Depth | CF_Stencil, ColorRGBA(0, 0, 0, 0), 1.0f, 0);

	// Todo: update render queue with render bucket filter
	mSceneMan->UpdateRenderQueue(mCamera, RO_None, RenderQueue::BucketAll, 0);   

	auto aabb = mSceneMan->GetRootSceneNode()->GetWorldBoundingBox();

	const RenderBucket& opaqueBucket = mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketOpaque);
	for (const RenderQueueItem& renderItem : opaqueBucket) 
	{
		renderItem.Renderable->GetMaterial()->SetCurrentTechnique("GBuffer");
		renderItem.Renderable->Render();
	}

	//if ( InputSystem::GetSingleton().MouseButtonPress(MS_MiddleButton) )
	//{
	//	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer0.pfm", mGBuffer[0]);
	//	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer1.tga", mGBuffer[1]);
	//}
}

void DeferredPath::DeferredLighting()
{
	// Keep a copy of invViewProjection matrix
	const float4x4& proj = mCamera->GetProjMatrix();

	mViewProj = mCamera->GetEngineViewProjMatrix();
	if (Application::msApp->GetAppSettings().RHDeviceType == RD_Direct3D11)
    {
		mInvViewProj = MatrixInverse(mViewProj);
	}
	else
	{
		const float4x4& view = mCamera->GetViewMatrix();
		mInvViewProj = MatrixInverse(view * proj);
	}

	mDevice->BindFrameBuffer(mLightAccumulateFB);
	mLightAccumulateRTV->ClearColor(ColorRGBA(0, 0, 0, 0));

	// Copy depth and stencil
	mDepthStencilBuffer->CopyToTexture(*mDepthStencilBufferLight);

	// Set all common effect parameters
	mDeferredEffect->GetParameterByName("InvViewProj")->SetValue(mInvViewProj);
	if (mDeferredEffect->GetParameterByName("CameraOrigin"))
		mDeferredEffect->GetParameterByName("CameraOrigin")->SetValue(mCamera->GetPosition());

 	bool stencilZFail = false;
	for (Light* light : mSceneMan->GetSceneLights())
	{
		LightType lightType = light->GetLightType();
		
		if (lightType == LT_DirectionalLight)
		{
			DrawDirectionalLightShape(light);
		}
		else 
		{
			if (!stencilZFail)
			{
				mDepthStencilBufferLightView->ClearStencil(0);
				stencilZFail = true;
			}	

			if (lightType == LT_SpotLight)
				DrawSpotLightShape(light);
			else if (lightType == LT_PointLight)
				DrawPointLightShape(light);
		}
	}

	/*if ( InputSystem::GetSingleton().MouseButtonPress(MS_MiddleButton) )
	{
	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer0.pfm", mGBuffer[0]);
	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer1.tga", mGBuffer[1]);
	mDevice->GetRenderFactory()->SaveTextureToFile("E:/LightAccu.pfm", mLightAccumulateBuffer);
	}

	if ( InputSystem::GetSingleton().MouseButtonPress(MS_MiddleButton) )
	{
		mDevice->GetRenderFactory()->SaveLinearDepthTextureToFile("E:/depth.pfm", mDepthStencilBufferLight, proj.M33, proj.M43);
		mDevice->GetRenderFactory()->SaveTextureToFile("E:/LightAccu.pfm", mLightAccumulateBuffer);
	}*/
}

void DeferredPath::DeferredShading()
{
	mDevice->BindFrameBuffer(mHDRFB);
	mHDRBufferRTV->ClearColor(ColorRGBA(0, 0, 0, 0));
	
	// Draw Sky box first
	const RenderBucket& bkgBucket = mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketBackground, false);
	for (const RenderQueueItem& item : bkgBucket)
		item.Renderable->Render();

	mDevice->Draw(mShadingTech, mFullscreenTrangle);

	/*if ( InputSystem::GetSingleton().MouseButtonPress(MS_MiddleButton) )
	{
		mDevice->GetRenderFactory()->SaveTextureToFile("E:/Light.pfm", mLightAccumulateBuffer);
		mDevice->GetRenderFactory()->SaveTextureToFile("E:/HDRBuffer.pfm", mHDRBuffer);
	}*/
}

void DeferredPath::PostProcess()
{
	shared_ptr<FrameBuffer> screenFB = mDevice->GetScreenFrameBuffer();
	mDevice->BindFrameBuffer(screenFB);
	screenFB->Clear(CF_Color | CF_Depth, ColorRGBA::Black, 1.0, 0);

	EffectTechnique* toneMapTech = mToneMapEffect->GetTechniqueByName("CopyColorDepth");
	mDevice->Draw(toneMapTech, mFullscreenTrangle);
}

void DeferredPath::DrawDirectionalLightShape( Light* light )
{
	bool bCastShadow = light->GetCastShadow();
	if (bCastShadow)
	{
		// Generate shadow map
		mShadowMan->MakeCascadedShadowMap(*light);
	}

	float3 lightColor = light->GetLightColor() * light->GetLightIntensity();
	mDeferredEffect->GetParameterByUsage(EPU_Light_Color)->SetValue(lightColor);

	const float3& worldDirection = light->GetDerivedDirection();
	float4 lightDir(worldDirection[0], worldDirection[1], worldDirection[2], 0.0f);
	mDeferredEffect->GetParameterByUsage(EPU_Light_Dir)->SetValue(lightDir);

	//mDeferredEffect->GetParameterByName("ShadowEnabled")->SetValue(bCastShadow);
	//if (bCastShadow)
	//{	
	//	mDeferredEffect->GetParameterByName("CascadeShadowTex")->SetValue(mShadowMan->mShadowTexture->GetShaderResourceView());
	//	mDeferredEffect->GetParameterByName("ShadowView")->SetValue(mShadowMan->mShadowView);
	//	mDeferredEffect->GetParameterByName("NumCascades")->SetValue((int)light->GetShadowCascades());
	//	mDeferredEffect->GetParameterByName("BorderPaddingMinMax")->SetValue(mShadowMan->mBorderPaddingMinMax);
	//	mDeferredEffect->GetParameterByName("CascadeScale")->SetValue(&(mShadowMan->mShadowCascadeScale[0]), MAX_CASCADES);
	//	mDeferredEffect->GetParameterByName("CascadeOffset")->SetValue(&(mShadowMan->mShadowCascadeOffset[0]), MAX_CASCADES); 
	//	//effect->GetParameterByName("CascadeBlendArea")->SetValue(mCascadedShadowMap->mCascadeBlendArea);
	//}

	mDevice->Draw(mDirLightTech, mFullscreenTrangle);
}

void DeferredPath::DrawSpotLightShape( Light* light )
{
	bool bCastShadow = light->GetCastShadow();
	if (bCastShadow)
		mShadowMan->MakeSpotShadowMap(*light);

	float3 lightColor = light->GetLightColor() * light->GetLightIntensity();
	const float3& worldPos = light->GetDerivedPosition();
	const float3& worldDir = light->GetDerivedDirection();

	float4 lightPos = float4(worldPos[0], worldPos[1], worldPos[2], 1.0f);
	float4 lightDir = float4(worldDir[0], worldDir[1], worldDir[2], 0.0f);
	
	float spotInnerAngle = light->GetSpotInnerAngle();
	float spotOuterAngle = light->GetSpotOuterAngle();
	lightPos[3] = cosf(spotInnerAngle);
	lightDir[3] = cosf(spotOuterAngle);

	if (mDeferredEffect->GetParameterByUsage(EPU_Light_Color))
	{
		mDeferredEffect->GetParameterByUsage(EPU_Light_Color)->SetValue(lightColor);
		mDeferredEffect->GetParameterByUsage(EPU_Light_Position)->SetValue(lightPos);
		mDeferredEffect->GetParameterByUsage(EPU_Light_Dir)->SetValue(lightDir);
		mDeferredEffect->GetParameterByUsage(EPU_Light_Attenuation)->SetValue(light->GetAttenuation());
	}


	float scaleHeight = light->GetRange();
	float scaleBase = scaleHeight * tanf(spotOuterAngle * 0.5f);

	float3 rotAxis = Cross(float3(0, 1, 0), worldDir);
	//float rotAngle = acosf(Dot(float3(0, 1, 0), worldDirection));
	float4x4 rotation = CreateRotationAxis(rotAxis, acosf(worldDir.Y()));

	float4x4 worldMatrix = CreateScaling(scaleBase, scaleHeight, scaleBase) * rotation *
						   CreateTranslation(worldPos);
	float4x4 worldViewProj = worldMatrix * mViewProj;
	mDeferredEffect->GetParameterByName("WorldViewProj")->SetValue(worldViewProj);

	/*mDeferredEffect->GetParameterByName("ShadowEnabled")->SetValue(bCastShadow);
	if (bCastShadow)
	{		
	mDeferredEffect->GetParameterByName("ShadowTex")->SetValue(mShadowMan->mShadowDepth->GetShaderResourceView());
	mDeferredEffect->GetParameterByName("ShadowViewProj")->SetValue(mShadowMan->mShadowView);
	if (mDeferredEffect->GetParameterByName("PCFRadius"))
	mDeferredEffect->GetParameterByName("PCFRadius")->SetValue(5.7f / float(SHADOW_MAP_SIZE));
	}*/


	mDevice->Draw(mSpotLightTech, mSpotLightShape);
}

void DeferredPath::DrawPointLightShape(Light* light )
{
	const Camera& currCamera = *(mDevice->GetCurrentFrameBuffer()->GetCamera());
	const float4x4& View = currCamera.GetViewMatrix();

	const float3& worldPos = light->GetDerivedPosition();
	float4 lightPos = float4(worldPos[0], worldPos[1], worldPos[2], 1.0f) ;
	float lightRadius = light->GetRange();

	float3 lightColor = light->GetLightColor() * light->GetLightIntensity();
	mDeferredEffect->GetParameterByUsage(EPU_Light_Color)->SetValue(lightColor);

	mDeferredEffect->GetParameterByUsage(EPU_Light_Position)->SetValue(lightPos);
	mDeferredEffect->GetParameterByUsage(EPU_Light_Attenuation)->SetValue(light->GetAttenuation());

	float4x4 worldMatrix(lightRadius,  0.0f,		  0.0f,		    0.0f,
						 0.0f,         lightRadius,   0.0f,		    0.0f,
						 0.0f,         0.0f,          lightRadius,  0.0f,
						 worldPos.X(), worldPos.Y(),  worldPos.Z(), 1.0f);

	float4x4 worldViewProj = worldMatrix * mViewProj;
	mDeferredEffect->GetParameterByName("WorldViewProj")->SetValue(worldViewProj);

	mDevice->Draw(mPointLightTech, mPointLightShape);
}

//----------------------------------------------------------------------------------------------
TiledDeferredPath::TiledDeferredPath()
{

}

TiledDeferredPath::~TiledDeferredPath()
{

}

void TiledDeferredPath::OnGraphicsInit( const shared_ptr<Camera>& camera )
{
	RenderPath::OnGraphicsInit(camera);

	Window* appWindow = Application::msApp->GetMainWindow();
	const uint32_t windowWidth = appWindow->GetWidth();
	const uint32_t windowHeight = appWindow->GetHeight();

	RenderFactory* factory = mDevice->GetRenderFactory();
	ResourceManager& resMan = ResourceManager::GetSingleton();

	// Load deferred lighting effect
	mTiledDeferredEffect = resMan.GetResourceByName<Effect>(RT_Effect, "TiledDeferredShading.effect.xml", "General");
	mToneMapEffect = resMan.GetResourceByName<Effect>(RT_Effect, "HDR.effect.xml", "General");

	mTileTech = mTiledDeferredEffect->GetTechniqueByName("Tile");
	mShadingTech = mTiledDeferredEffect->GetTechniqueByName("Shading");

	// Init GBuffer
	mGBufferFB = factory->CreateFrameBuffer(windowWidth, windowHeight);
	mGBufferFB->SetViewport(Viewport(0.0f, 0.f, float(windowWidth), float(windowHeight)));

#ifdef _DEBUG
	uint32_t acessHint = EAH_GPU_Write | EAH_GPU_Read;
#else
	uint32_t acessHint = EAH_GPU_Write | EAH_GPU_Read;
#endif

	uint32_t rtCreateFlag = TexCreate_ShaderResource | TexCreate_RenderTarget;
	uint32_t dsCreateFlag = TexCreate_ShaderResource | TexCreate_DepthStencilTarget;

	mGBuffer[0] = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA16F, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mGBuffer[1] = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA8_UNORM, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mDepthStencilBuffer = factory->CreateTexture2D(windowWidth, windowHeight, PF_D24S8, 1, 1, 1, 0, acessHint, dsCreateFlag, NULL);

	mGBufferRTV[0] = factory->CreateRenderTargetView2D(mGBuffer[0], 0, 0);
	mGBufferRTV[1] = factory->CreateRenderTargetView2D(mGBuffer[1], 0, 0);
	mDepthStencilView = factory->CreateDepthStencilView(mDepthStencilBuffer, 0, 0);

	mGBufferFB->AttachRTV(ATT_DepthStencil, mDepthStencilView);
	mGBufferFB->AttachRTV(ATT_Color0, mGBufferRTV[0]);
	mGBufferFB->AttachRTV(ATT_Color1, mGBufferRTV[1]);
	mGBufferFB->SetCamera(camera);

	// Init light buffer
	mLightAccumulation = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA32F, 1, 1, 1, 0, acessHint, rtCreateFlag | TexCreate_UAV, NULL);
	mLightAccumulationUAV = factory->CreateTexture2DUAV(mLightAccumulation, 0, 0, 1);

	// HDR buffer
	mHDRFB = factory->CreateFrameBuffer(windowWidth, windowHeight);
	mHDRFB->SetViewport(Viewport(0.0f, 0.f, float(windowWidth), float(windowHeight)));

	mHDRBuffer = factory->CreateTexture2D(windowWidth, windowHeight, PF_RGBA32F, 1, 1, 1, 0, acessHint, rtCreateFlag, NULL);
	mHDRBufferRTV = factory->CreateRenderTargetView2D(mHDRBuffer, 0, 0);
	mHDRFB->AttachRTV(ATT_Color0, mHDRBufferRTV);

	uint32_t dsvReadOnly = DSVCreate_ReadOnly_Stencil | DSVCreate_ReadOnly_Depth;
	mDepthStencilViewReadOnly = factory->CreateDepthStencilView(mDepthStencilBuffer, 0, 0, dsvReadOnly);
	mHDRFB->AttachRTV(ATT_DepthStencil, mDepthStencilViewReadOnly);

	// Create structure buffer for lights
	mLightBuffer = factory->CreateStructuredBuffer(sizeof(PointLight), MaxNumLights, EAH_GPU_Read | EAH_CPU_Write, BufferCreate_Structured | BufferCreate_ShaderResource, nullptr);
	mLightBufferSRV = factory->CreateStructuredBufferSRV(mLightBuffer, 0, MaxNumLights, sizeof(PointLight));

	// Init shadow manager
	//mShadowMan = new CascadedShadowMap(mDevice);

	// bind shader input
	mTiledDeferredEffect->GetParameterByName("GBuffer0")->SetValue(mGBuffer[0]->GetShaderResourceView());
	mTiledDeferredEffect->GetParameterByName("GBuffer1")->SetValue(mGBuffer[1]->GetShaderResourceView());
	mTiledDeferredEffect->GetParameterByName("DepthBuffer")->SetValue(mDepthStencilBuffer->GetShaderResourceView());
	mTiledDeferredEffect->GetParameterByName("RWLightAccumulation")->SetValue(mLightAccumulationUAV);
	mTiledDeferredEffect->GetParameterByName("LightAccumulateBuffer")->SetValue(mLightAccumulation->GetShaderResourceView());
	mTiledDeferredEffect->GetParameterByName("Lights")->SetValue(mLightBufferSRV);

	mToneMapEffect->GetParameterByName("HDRBuffer")->SetValue(mHDRBuffer->GetShaderResourceView());	
	//mToneMapEffect->GetParameterByName("DepthBuffer")->SetValue(mDepthStencilBuffer->GetShaderResourceView());
}

void TiledDeferredPath::OnWindowResize( uint32_t width, uint32_t height )
{

}

void TiledDeferredPath::RenderScene()
{
	// Stage 0, generate GBuffer
	GenereateGBuffer();

	// Stage 1, tiled lighting 
	TiledDeferredLighting();

	// Stage 2, deferred shading
	DeferredShading();


	shared_ptr<FrameBuffer> screenFB = mDevice->GetScreenFrameBuffer();
	mDevice->BindFrameBuffer(screenFB);
	screenFB->Clear(CF_Color | CF_Depth, ColorRGBA(1, 0, 1, 1), 1.0, 0);

	EffectTechnique* toneMapTech = mToneMapEffect->GetTechniqueByName("CopyColor");
	mDevice->Draw(toneMapTech, mFullscreenTrangle);
}

void TiledDeferredPath::GenereateGBuffer()
{
	mDevice->BindFrameBuffer(mGBufferFB);
	mGBufferFB->Clear(CF_Color | CF_Depth | CF_Stencil, ColorRGBA(0, 0, 0, 0), 1.0f, 0);

	// Todo: update render queue with render bucket filter
	shared_ptr<Camera> camera = mGBufferFB->GetCamera();
	mSceneMan->UpdateRenderQueue(camera, RO_None, RenderQueue::BucketAll, 0);   

	const RenderBucket& opaqueBucket = mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketOpaque);
	for (const RenderQueueItem& renderItem : opaqueBucket) 
	{
		renderItem.Renderable->GetMaterial()->SetCurrentTechnique("GBuffer");
		renderItem.Renderable->Render();
	}

	//if ( InputSystem::GetSingleton().MouseButtonPress(MS_MiddleButton) )
	//{
	//	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer0.pfm", mGBuffer[0]);
	//	mDevice->GetRenderFactory()->SaveTextureToFile("E:/GBuffer1.tga", mGBuffer[1]);
	//}
}

void TiledDeferredPath::TiledDeferredLighting()
{
	uint32_t windowWidth = mLightAccumulation->GetWidth();
	uint32_t windowHeight = mLightAccumulation->GetHeight();
	
	mSceneMan->UpdateLightQueue(*mCamera);
	const LightQueue& sceneLights = mSceneMan->GetLightQueue();

	uint32_t numTotalCount = 0;

	// Fill light data
	if (sceneLights.size())
	{
		PointLight* pLights = reinterpret_cast<PointLight*>( mLightBuffer->Map(0, sizeof(PointLight) * sceneLights.size(), RMA_Write_Discard) );

		for (Light* light : sceneLights)
		{
			if (light->GetLightType() == LT_PointLight)
			{
				pLights[numTotalCount].Color = light->GetLightColor() * light->GetLightIntensity();
				pLights[numTotalCount].Position = light->GetPosition();
				pLights[numTotalCount].Range = light->GetRange();
				pLights[numTotalCount].Falloff = light->GetAttenuation();

				numTotalCount++;
			}
		}
		mLightBuffer->UnMap();
	}
	
	const float4x4& view = mCamera->GetViewMatrix();
	const float4x4& proj = mCamera->GetProjMatrix();
	const float4x4 invViewProj = MatrixInverse(view * proj);
	const float4x4 invProj = MatrixInverse(proj);
	float4 viewDim = float4((float)windowWidth, (float)windowHeight, 1.0f / windowWidth, 1.0f / windowHeight);

	mTiledDeferredEffect->GetParameterByName("View")->SetValue(view);
	mTiledDeferredEffect->GetParameterByName("Projection")->SetValue(proj);
	mTiledDeferredEffect->GetParameterByName("InvProj")->SetValue(invProj);
	mTiledDeferredEffect->GetParameterByName("InvViewProj")->SetValue(invViewProj);
	mTiledDeferredEffect->GetParameterByName("ViewportDim")->SetValue(viewDim);
	mTiledDeferredEffect->GetParameterByName("ProjRatio")->SetValue(float2(proj.M33, proj.M43));
	mTiledDeferredEffect->GetParameterByName("CameraOrigin")->SetValue(mCamera->GetPosition());
	mTiledDeferredEffect->GetParameterByName("LightCount")->SetValue(numTotalCount);

	uint32_t numGroupX = (windowWidth + TileGroupSize - 1) / TileGroupSize;
	uint32_t numGroupY = (windowHeight + TileGroupSize - 1) / TileGroupSize;

	mLightAccumulationUAV->Clear(float4(0, 0, 0, 0));
	
	mDevice->DispatchCompute(mTileTech, numGroupX, numGroupY, 1);

	//mDevice->GetRenderFactory()->SaveLinearDepthTextureToFile("E:/depth.pfm", mDepthStencilBuffer, proj.M33, proj.M43);
	//mDevice->GetRenderFactory()->SaveTextureToFile("E:/Light.pfm", mLightAccumulation);
}

void TiledDeferredPath::DeferredShading()
{
	mDevice->BindFrameBuffer(mHDRFB);
	mHDRBufferRTV->ClearColor(ColorRGBA(0, 0, 0, 0));
	
	// Draw Sky box first
	const RenderBucket& bkgBucket = mSceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketBackground, false);
	for (const RenderQueueItem& item : bkgBucket)
		item.Renderable->Render();

	mDevice->Draw(mShadingTech, mFullscreenTrangle);

	//mDevice->GetRenderFactory()->SaveTextureToFile("E:/Light.pfm", mLightAccumulation);
	//mDevice->GetRenderFactory()->SaveTextureToFile("E:/HDRBuffer.pfm", mHDRBuffer);
}






}

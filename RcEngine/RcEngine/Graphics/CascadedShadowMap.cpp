#include <Graphics/CascadedShadowMap.h>
#include <Scene/SceneManager.h>
#include <Scene/SceneNode.h>
#include <Scene/Light.h>
#include <Graphics/RenderDevice.h>
#include <Graphics/RenderFactory.h>
#include <Graphics/GraphicsResource.h>
#include <Graphics/VertexDeclaration.h>
#include <Graphics/FrameBuffer.h>
#include <Graphics/Effect.h>
#include <Graphics/Camera.h>
#include <Graphics/Material.h>
#include <Graphics/RenderOperation.h>
#include <Graphics/Geometry.h>
#include <Resource/ResourceManager.h>
#include <Core/Environment.h>
#include <Math/MathUtil.h>
#include <Graphics/pfm.h>
#include <Graphics/DebugDrawManager.h>
#include <MainApp/Application.h>

namespace {

using namespace RcEngine;

void FrustumPlaneExtraction(const Camera& camera, float camZ, float3 corner[4])
{
	float height = camZ * tanf(camera.GetFov() * 0.5f);
	float width = height * camera.GetAspect();

	float3 center = camera.GetPosition() + camera.GetForward() * camZ;
	float3 right = camera.GetRight();
	float3 up = camera.GetUp();

	corner[0] = center - width * right - height * up;
	corner[1] = center + width * right - height * up;
	corner[2] = center - width * right + height * up;
	corner[3] = center + width * right + height * up;
}

//--------------------------------------------------------------------------------------
// Computing an accurate near and far plane will decrease surface acne and Peter-panning.
// Surface acne is the term for erroneous self shadowing.  Peter-panning is the effect where
// shadows disappear near the base of an object.
// As offsets are generally used with PCF filtering due self shadowing issues, computing the
// correct near and far planes becomes even more important.
// This concept is not complicated, but the intersection code is.
//--------------------------------------------------------------------------------------
void CalculateLightNearFar( BoundingBoxf& lightFrustumBound, const float3 sceneAABBPointLightSpace[8] )
{
	// By intersecting the light frustum with the scene AABB we can get a tighter bound on the near and far plane.
	struct Triangle
	{
		float3 pt[3];
		bool culled;
	};

	// Initialize the near and far planes
	float nearPlane = FLT_MAX;
	float farPlane  = -FLT_MAX;

	Triangle triangleList[16];
	int iTriangleCnt = 1;

	// These are the indices used to tesselate an AABB into a list of triangles.
	static const int AABBTriIndexes[] = 
	{
		0,1,2,  1,2,3,
		4,5,6,  5,6,7,
		0,2,4,  2,4,6,
		1,3,5,  3,5,7,
		0,1,4,  1,4,5,
		2,3,6,  3,6,7 
	};

	// Four clip plane
	const float ClipPlaneEdge[4] = { 
		lightFrustumBound.Min.X(), 
		lightFrustumBound.Max.X(), 
		lightFrustumBound.Min.Y(), 
		lightFrustumBound.Max.Y()
	};

	// At a high level: 
	// 1. Iterate over all 12 triangles of the AABB.  
	// 2. Clip the triangles against each plane. Create new triangles as needed.
	// 3. Find the min and max z values as the near and far plane.

	//This is easier because the triangles are in camera spacing making the collisions tests simple comparisions.
	int pointPassClip[3];

	for( int iAABBTri = 0; iAABBTri < 12; ++iAABBTri ) 
	{
		triangleList[0].pt[0] = sceneAABBPointLightSpace[ AABBTriIndexes[ iAABBTri*3 + 0 ] ];
		triangleList[0].pt[1] = sceneAABBPointLightSpace[ AABBTriIndexes[ iAABBTri*3 + 1 ] ];
		triangleList[0].pt[2] = sceneAABBPointLightSpace[ AABBTriIndexes[ iAABBTri*3 + 2 ] ];
		triangleList[0].culled = false;
		iTriangleCnt = 1;

		// Clip each invidual triangle against the 4 frustums.  When ever a triangle is clipped into new triangles, 
		//add them to the list.
		for( int iFrustumPlane = 0; iFrustumPlane < 4; ++iFrustumPlane ) 
		{
			float edge = ClipPlaneEdge[iFrustumPlane];
			int component = iFrustumPlane >> 1;

			for( int iTri=0; iTri < iTriangleCnt; ++iTri ) 
			{
				// We don't delete triangles, so we skip those that have been culled.
				if( !triangleList[iTri].culled ) 
				{
					int insideVertCount = 0;

					// Test against the correct frustum plane.

					if (iFrustumPlane % 2 == 0) // For MinX, MinY
					{		
						for( int iTriPt=0; iTriPt < 3; ++iTriPt ) 
						{
							pointPassClip[iTriPt] = (triangleList[iTri].pt[iTriPt][component] > edge) ? 1 : 0;
							insideVertCount += pointPassClip[iTriPt];
						}
					}
					else // For MaxX, MaxY
					{
						for( int iTriPt=0; iTriPt < 3; ++iTriPt ) 
						{
							pointPassClip[iTriPt] = (triangleList[iTri].pt[iTriPt][component] < edge) ? 1 : 0;
							insideVertCount += pointPassClip[iTriPt];
						}
					}			

					// Move the points that pass the frustum test to the begining of the array.
					if( pointPassClip[1] && !pointPassClip[0] ) 
					{
						std::swap(triangleList[iTri].pt[0], triangleList[iTri].pt[1]);
						std::swap(pointPassClip[0], pointPassClip[1]); 
					}
					if( pointPassClip[2] && !pointPassClip[1] ) 
					{
						std::swap(triangleList[iTri].pt[1], triangleList[iTri].pt[2]);
						std::swap(pointPassClip[1], pointPassClip[2]);                    
					}
					if( pointPassClip[1] && !pointPassClip[0] ) 
					{
						std::swap(triangleList[iTri].pt[0], triangleList[iTri].pt[1]);
						std::swap(pointPassClip[0], pointPassClip[1]);    
					}

					if( insideVertCount == 0 ) 
					{ // All points failed. We're done,  
						triangleList[iTri].culled = true;
					}
					else if( insideVertCount == 1 ) 
					{// One point passed. Clip the triangle against the Frustum plane
						triangleList[iTri].culled = false;				

						float hitRatio = edge - triangleList[iTri].pt[0][component];
						float ratio01 = hitRatio / (triangleList[iTri].pt[1][component] - triangleList[iTri].pt[0][component]);
						float ratio02 = hitRatio / (triangleList[iTri].pt[2][component] - triangleList[iTri].pt[0][component]);

						float3 v1 = Lerp(triangleList[iTri].pt[0], triangleList[iTri].pt[1], ratio01);
						float3 v2 = Lerp(triangleList[iTri].pt[0], triangleList[iTri].pt[2], ratio02);

						triangleList[iTri].pt[1] = v2;
						triangleList[iTri].pt[2] = v1;
					}
					else if( insideVertCount == 2 ) 
					{ // 2 in  // tesselate into 2 triangles

						// Copy the triangle\(if it exists) after the current triangle out of
						// the way so we can override it with the new triangle we're inserting.
						triangleList[iTriangleCnt] = triangleList[iTri+1];

						triangleList[iTri].culled = false;
						triangleList[iTri+1].culled = false;

						// Get the hit point ratio.
						float fitRatio =  edge - triangleList[iTri].pt[2][component];
						float ratio20 = fitRatio / (triangleList[iTri].pt[0][component] - triangleList[iTri].pt[2][component]);
						float ratio21 = fitRatio / (triangleList[iTri].pt[1][component] - triangleList[iTri].pt[2][component]);

						float3 v2 = Lerp(triangleList[iTri].pt[2], triangleList[iTri].pt[0], ratio20);
						float3 v1 = Lerp(triangleList[iTri].pt[2], triangleList[iTri].pt[1], ratio21);

						// Add new triangles.
						triangleList[iTri+1].pt[0] = triangleList[iTri].pt[0];
						triangleList[iTri+1].pt[1] = triangleList[iTri].pt[1];
						triangleList[iTri+1].pt[2] = v2;

						triangleList[iTri].pt[0] = triangleList[iTri+1].pt[1];
						triangleList[iTri].pt[1] = triangleList[iTri+1].pt[2];
						triangleList[iTri].pt[2] = v1;

						// Increment triangle count and skip the triangle we just inserted.
						++iTriangleCnt;
						++iTri;
					}
					else 
					{ // all in
						triangleList[iTri].culled = false;
					}
				}// end if !culled loop            
			}
		}

		for( int index=0; index < iTriangleCnt; ++index ) 
		{
			if( !triangleList[index].culled ) 
			{
				// Set the near and far plan and the min and max z values respectivly.
				for( int iVert = 0; iVert < 3; ++ iVert ) 
				{
					float vertCoordZ = triangleList[index].pt[iVert].Z();

					if( nearPlane > vertCoordZ ) nearPlane = vertCoordZ;
					if( farPlane  <vertCoordZ )  farPlane = vertCoordZ;
				}
			}
		}
	}    

	lightFrustumBound.Min.Z() = nearPlane;
	lightFrustumBound.Max.Z() = farPlane;
}


}

namespace RcEngine {

CascadedShadowMap::CascadedShadowMap(RenderDevice* device)
	: mDevice(device),
	  mMoveLightTexelSize(true)
{
	mShadowCascadeScale.resize(MAX_CASCADES);
	mShadowCascadeOffset.resize(MAX_CASCADES);
	for (int i = 0; i < MAX_CASCADES; ++i)
		mLightCamera.push_back(std::make_shared<Camera>());

	// Load shade map blur effect
	ResourceManager& resMan = ResourceManager::GetSingleton();
	mBlurEffect = resMan.GetResourceByName<Effect>(RT_Effect, "ShadowMapBlur.effect.xml", "General");
	
	BuildFullscreenTriangle(mFSQuadRop);

	//mBorderPaddingMinMax = float2(0.0, 1.0);
	mBorderPaddingMinMax = float2(float(1)/float(SHADOW_MAP_SIZE), float(SHADOW_MAP_SIZE-1)/float(SHADOW_MAP_SIZE));
	mCascadeBlendArea = mBorderPaddingMinMax[1];

	float scaleY = (Application::msApp->GetAppSettings().RHDeviceType == RD_Direct3D11) ? -0.5f : 0.5f;
	mShadowTextureNorm = CreateScaling(0.5f, scaleY, 1.0f) * CreateTranslation(0.5f, 0.5f, 0.0f);
}

void CascadedShadowMap::UpdateShadowMapStorage( const Light& light )
{
	RenderFactory* factory = mDevice->GetRenderFactory();

#ifdef _DEBUG
	uint32_t accessHint = EAH_CPU_Read | EAH_GPU_Read | EAH_GPU_Write;
#else
	uint32_t accessHint = EAH_GPU_Read | EAH_GPU_Write;
#endif
	uint32_t texCreateFlags = TexCreate_RenderTarget | TexCreate_ShaderResource;
	
	if (!mShadowFrameBuffer)
	{
		mShadowFrameBuffer = factory->CreateFrameBuffer(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
		mShadowFrameBuffer->SetViewport(0, Viewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE));

		mShadowDepth = factory->CreateTexture2D(
			SHADOW_MAP_SIZE, 
			SHADOW_MAP_SIZE,
			PF_D32F, 
			1,
			1,
			1, 0, 
			accessHint,
			TexCreate_DepthStencilTarget | TexCreate_ShaderResource,
			NULL);

		mShadowFrameBuffer->AttachRTV(ATT_DepthStencil, factory->CreateDepthStencilView(mShadowDepth, 0, 0));
	}
	
	if (light.GetLightType() == LT_DirectionalLight)
	{
		uint32_t numCascade = light.GetShadowCascades();

		PixelFormat ShadowMapFormat = PF_R32F;

#if defined(SHADOW_MAP_VSM)
		ShadowMapFormat = PF_RG32F;
#elif defined(SHADOW_MAP_EVSM)
		ShadowMapFormat = PF_RGBA32F
#elif defined(SHADOW_MAP_ESM)
		ShadowMapFormat = PF_R32F;
#endif
		if (!mShadowTexture || mShadowTexture->GetTextureArraySize() != numCascade)
		{
			mShadowTexture = factory->CreateTexture2D(
				SHADOW_MAP_SIZE, 
				SHADOW_MAP_SIZE, 
				ShadowMapFormat,
				numCascade,
				0,		// Auto Mipmap Generate 
				1, 0,	// Sample Count
				accessHint,
				TexCreate_RenderTarget | TexCreate_ShaderResource | TexCreate_GenerateMipmaps,
				NULL);
			
			mShadowSplitsRTV.clear();
			for (uint32_t i = 0; i < numCascade; ++i)
				mShadowSplitsRTV.push_back(factory->CreateRenderTargetView2D(mShadowTexture, i, 0));

			// VSM blur buffer
			mShadowMapTempBlur = factory->CreateTexture2D(
				SHADOW_MAP_SIZE,
				SHADOW_MAP_SIZE, 
				ShadowMapFormat,
				1,
				1,
				1, 0,
				accessHint,
				texCreateFlags,
				NULL);
			
			mShadowMapTempBlurRTV = factory->CreateRenderTargetView2D(mShadowMapTempBlur, 0, 0);

			//mShadowMapTempBlur1 = factory->CreateTexture2D(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, shadowTexFmt, 1, 1, 1, 0, EAH_GPU_Write | EAH_GPU_Read | EAH_CPU_Read, NULL);
			//mShadowMapTempBlurRTV1 = factory->CreateRenderTargetView2D(mShadowMapTempBlur1, 0, 0);
		}
	}
	else if (light.GetLightType() == LT_PointLight)
	{
		// Onmi-light, six shadow map
	}
}

void CascadedShadowMap::MakeCascadedShadowMap(const Light& light)
{
	RenderDevice* device = Environment::GetSingleton().GetRenderDevice();
	SceneManager* sceneMan = Environment::GetSingleton().GetSceneManager();

	const shared_ptr<FrameBuffer>& currFrameBuffer = device->GetCurrentFrameBuffer();
	const Camera& viewCamera = *currFrameBuffer->GetCamera();

	 // Create shadow texture if not. or resize it if shadow map size change.
	UpdateShadowMapStorage(light);

	// Update shadow map matrix
	UpdateShadowMatrix(viewCamera, light);	

	const String shadowMapTech = "VSM";
	
	// Draw all shadow map
	for (uint32_t i = 0; i < light.GetShadowCascades(); ++i)
	{		
		mShadowFrameBuffer->SetCamera(mLightCamera[i]);

		auto viewProj = mLightCamera[i]->GetEngineViewProjMatrix();
	
		mShadowFrameBuffer->AttachRTV(ATT_Color0, mShadowSplitsRTV[i]);
		mShadowFrameBuffer->SetViewport(0, Viewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE));
		mDevice->BindFrameBuffer(mShadowFrameBuffer);	

		mShadowFrameBuffer->Clear(CF_Depth | CF_Color, ColorRGBA(1, 1, 1, 1), 1.0f, 0);

		// Update light render queue 
		sceneMan->UpdateRenderQueue(mLightCamera[i], RO_None, 
			RenderQueue::BucketOpaque | RenderQueue::BucketTransparent, SceneObject::NoCastShadow);

		RenderBucket& opaqueBucket = sceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketOpaque);
		for (const RenderQueueItem& renderItem : opaqueBucket) 
		{
			renderItem.Renderable->GetMaterial()->SetCurrentTechnique(shadowMapTech);
			renderItem.Renderable->Render();
		}
	}

	//uint32_t level = 0;
	//uint32_t shadowMapSize = SHADOW_MAP_SIZE >> level;
	//vector<float> pfmData(shadowMapSize * shadowMapSize);

	//for (size_t layer = 0; layer < light.GetShadowCascades(); ++layer)
	//{
	//	uint32_t rowPitch;
	//	float* pData = (float*)mShadowTexture->Map2D(layer, level, RMA_Read_Only, rowPitch);
	//	for (size_t iY = 0; iY < shadowMapSize; ++iY)
	//	{
	//		for (size_t iX = 0; iX < shadowMapSize; ++iX)
	//		{
	//			pfmData[iY * shadowMapSize + iX] = *pData;

	//			pData += 2;
	//		}
	//	}
	//	mShadowTexture->Unmap2D(layer, 0);

	//	String Name = "E:/PSSM" + std::to_string(layer) + ".pfm";
	//	WritePfm(Name.c_str(), shadowMapSize, shadowMapSize, 1, &pfmData[0]);
	//}

	// Blur shadow map
	for (uint32_t i = 0; i < light.GetShadowCascades(); ++i)
	{
		mShadowFrameBuffer->AttachRTV(ATT_Color0, mShadowMapTempBlurRTV);
		mDevice->BindFrameBuffer(mShadowFrameBuffer);	

		mBlurEffect->SetCurrentTechnique("BlurX");
		mBlurEffect->GetParameterByName("ShadowMap")->SetValue(mShadowTexture->GetShaderResourceView());
		mBlurEffect->GetParameterByName("ArraySlice")->SetValue(float(i));
		mDevice->Draw(mBlurEffect->GetCurrentTechnique(), mFSQuadRop);

		//String filename = "E:/BlurX" + std::to_string(i) + ".pfm";
		//mDevice->GetRenderFactory()->SaveTextureToFile(filename, mShadowMapTempBlur);

		mShadowFrameBuffer->AttachRTV(ATT_Color0, mShadowSplitsRTV[i]);
		mDevice->BindFrameBuffer(mShadowFrameBuffer);	

		mBlurEffect->SetCurrentTechnique("BlurY");
		mBlurEffect->GetParameterByName("ShadowMapBlurX")->SetValue(mShadowMapTempBlur->GetShaderResourceView());
		mDevice->Draw(mBlurEffect->GetCurrentTechnique(), mFSQuadRop);
	}
	//mShadowTexture->BuildMipMap();
	
	mDevice->BindFrameBuffer(currFrameBuffer);	

	//for (size_t layer = 0; layer < light.GetShadowCascades(); ++layer)
	//{
	//	uint32_t rowPitch;
	//	float* pData = (float*)mShadowTexture->Map2D(layer, level, RMA_Read_Only, rowPitch);
	//	for (size_t iY = 0; iY < shadowMapSize; ++iY)
	//	{
	//		for (size_t iX = 0; iX < shadowMapSize; ++iX)
	//		{
	//			pfmData[iY * shadowMapSize + iX] = *pData;

	//			pData += 2;
	//		}
	//	}
	//	mShadowTexture->Unmap2D(layer, 0);

	//	String Name = "E:/BlurY" + std::to_string(layer) + ".pfm";
	//	WritePfm(Name.c_str(), shadowMapSize, shadowMapSize, 1, &pfmData[0]);
	//}
}

void CascadedShadowMap::UpdateShadowMatrix( const Camera& camera, const Light& light )
{
	RenderDevice* device = Environment::GetSingleton().GetRenderDevice();
	SceneManager* sceneMan = Environment::GetSingleton().GetSceneManager();

	const float3& camPos = camera.GetPosition();
	const float3& camDir = camera.GetForward();
	const float3& camUp = camera.GetUp();

	const uint32_t numSplits= light.GetShadowCascades();
	const float splitLambda = light.GetSplitLambda();
	const float3& lightDirection = light.GetDerivedDirection();

	// Build light coordinate system, view matrix.
	float3 lightUp = camera.GetRight();
	if(fabs(Dot(lightUp,lightDirection))>0.9f) lightUp = camera.GetUp();

	for (uint32_t i = 0; i < numSplits; ++i)
		mLightCamera[i]->CreateLookAt(float3(0, 0, 0), lightDirection, lightUp);

	// Keep a copy
	mShadowView = mLightCamera[0]->GetViewMatrix();

	// 0. Find scene AABB and transform to light view space
	float3 sceneAABBPointsLightSpace[8];
	{
		BoundingBoxf sceneAABB = sceneMan->GetRootSceneNode()->GetWorldBoundingBox(); 
		sceneAABB.GetCorners(sceneAABBPointsLightSpace);

		// Transform the scene AABB to Light space.
		for (int i = 0; i < 8; ++i)
			sceneAABBPointsLightSpace[i] = Transform(sceneAABBPointsLightSpace[i], mShadowView);
	}
	
	// 1. Split view frustum into sub frustum
	float nearPlane = camera.GetNearPlane();
	float farPlane = camera.GetFarPlane();

	uint32_t iSplit = 0;
	mSplitPlanes[iSplit] = nearPlane;
	for (iSplit = 1; iSplit < numSplits; ++iSplit)
	{
		float f = float(iSplit) / numSplits;
		float logSplit = nearPlane * powf(farPlane/nearPlane, f);
		float uniSplit = nearPlane + (farPlane - nearPlane) * f;
		mSplitPlanes[iSplit] = splitLambda * logSplit + (1.0f - splitLambda) * uniSplit;
	}
	mSplitPlanes[iSplit] = farPlane;

	// 2. Compute an orthographic projection for each sub frustum.
	int32_t nearSplitIdx = 0, farSplitIdx = 1;
	float3 Corners[2][4];
	FrustumPlaneExtraction(camera, mSplitPlanes[0], Corners[nearSplitIdx]);

	// Transform world space sub-frustum corners to light view space
	for (int i = 0; i < 4; ++i) 
		Corners[nearSplitIdx][i] = Transform(Corners[nearSplitIdx][i], mShadowView);

	for (iSplit = 0; iSplit < numSplits; ++iSplit)
	{
		// Compute AABB for sub frustum, Fit to Cascade
		FrustumPlaneExtraction(camera, mSplitPlanes[iSplit+1], Corners[farSplitIdx]);

		for (int i = 0; i < 4; ++i)
			Corners[farSplitIdx][i] = Transform(Corners[farSplitIdx][i], mShadowView);

		BoundingBoxf boundSplit;
		for (int i = 0; i < 4; ++i)
		{
			boundSplit.Merge(Corners[nearSplitIdx][i]);
			boundSplit.Merge(Corners[farSplitIdx][i]);
		}

		float3 worldUnitsPerTexel = float3(0.0f);
		{
			// We calculate a looser bound based on the size of the blur kernel.  This ensures us that we're 
			// sampling within the correct map.
			float scaleDuetoBlureAMT = float(SHADOW_MAP_BLUR_KERNEL_SIZE) / float(SHADOW_MAP_SIZE);
			//float scaleDuetoBlureAMT = float(SHADOW_MAP_BLUR_KERNEL_SIZE/2) / float(SHADOW_MAP_SIZE);
		
			// We calculate the offsets as a percentage of the bound.
			float3 vBoarderOffset = (boundSplit.Max - boundSplit.Min) * 0.5 * scaleDuetoBlureAMT;

			boundSplit.Max += vBoarderOffset;
			boundSplit.Min -= vBoarderOffset;

			// The world units per texel are used to snap the orthographic projection to texel sized increments.  
			// Because we're fitting tighly to the cascades, the shimmering shadow edges will still be present when the 
			// camera rotates.  However, when zooming in or strafing the shadow edge will not shimmer.
			worldUnitsPerTexel = (boundSplit.Max - boundSplit.Min) / float(SHADOW_MAP_SIZE);
		}

		if( mMoveLightTexelSize ) 
		{
			// We snape the camera to 1 pixel increments so that moving the camera does not cause the shadows to jitter.
			// This is a matter of integer dividing by the world space size of a texel
			boundSplit.Min.X() /= worldUnitsPerTexel.X();
			boundSplit.Min.X() = floorf( boundSplit.Min.X()  );
			boundSplit.Min.X() *= worldUnitsPerTexel.X();
			boundSplit.Min.Y() /= worldUnitsPerTexel.Y();
			boundSplit.Min.Y() = floorf( boundSplit.Min.Y()  );
			boundSplit.Min.Y() *= worldUnitsPerTexel.Y();

			boundSplit.Max.X() /= worldUnitsPerTexel.X();
			boundSplit.Max.X() = floorf( boundSplit.Max.X()  );
			boundSplit.Max.X() *= worldUnitsPerTexel.X();
			boundSplit.Max.Y() /= worldUnitsPerTexel.Y();
			boundSplit.Max.Y() = floorf( boundSplit.Max.Y()  );
			boundSplit.Max.Y() *= worldUnitsPerTexel.Y();
		}

		CalculateLightNearFar(boundSplit, sceneAABBPointsLightSpace);

		// Build ortho projection matrix
		mLightCamera[iSplit]->CreateOrthoOffCenter(boundSplit.Min.X(), boundSplit.Max.X(),
												   boundSplit.Min.Y(), boundSplit.Max.Y(),
												   boundSplit.Min.Z(), boundSplit.Max.Z());

		float4x4 orthoProjection = mLightCamera[iSplit]->GetProjMatrix() * mShadowTextureNorm;
		mShadowCascadeScale[iSplit] = float4(orthoProjection.M11, orthoProjection.M22, orthoProjection.M33, 1.0f);
		mShadowCascadeOffset[iSplit] = float4(orthoProjection.M41, orthoProjection.M42, orthoProjection.M43, 0.0f);

		// Ping-Pang swap
		std::swap(nearSplitIdx, farSplitIdx);
	}
}

void CascadedShadowMap::MakeSpotShadowMap( const Light& light )
{
	UpdateShadowMapStorage(light);

	SceneManager* sceneMan = Environment::GetSingleton().GetSceneManager();

	const shared_ptr<FrameBuffer>& currFrameBuffer = mDevice->GetCurrentFrameBuffer();
	const Camera& viewCamera = *currFrameBuffer->GetCamera();

	float fov = light.GetSpotOuterAngle();
	float zFar = light.GetRange();

	const float3& lightPosition = light.GetDerivedPosition();
	const float3& lightDirection = light.GetDerivedDirection();

	// Build light coordinate system, view matrix.
	float3 lightUp = viewCamera.GetRight();
	if(fabs(Dot(lightUp,lightDirection))>0.9f) lightUp = viewCamera.GetUp();

	mLightCamera[0]->CreateLookAt(light.GetDerivedPosition(), lightPosition + light.GetDerivedDirection(), lightUp);
	mLightCamera[0]->CreatePerspectiveFov(fov, 1.0, light.GetSpotlightNearClip(), zFar);

	// Update light render queue 
	sceneMan->UpdateRenderQueue(mLightCamera[0], RO_None, 
		RenderQueue::BucketOpaque | RenderQueue::BucketTransparent, SceneObject::NoCastShadow);

	const String& shadowMapTech = "PCF";

	mShadowFrameBuffer->SetCamera(mLightCamera[0]);
	for (size_t i = 0; i < 8; ++i)
	{
		Attachment attachment = Attachment(ATT_Color0 + i);
		mShadowFrameBuffer->DetachRTV(attachment);  // Only Depth Attach Used
	}
	mDevice->BindFrameBuffer(mShadowFrameBuffer);
	mShadowFrameBuffer->Clear(CF_Depth, ColorRGBA::Black, 1.0, 0);

	RenderBucket& opaqueBucket = sceneMan->GetRenderQueue().GetRenderBucket(RenderQueue::BucketOpaque);	
	for (const RenderQueueItem& renderItem : opaqueBucket) 
	{
		renderItem.Renderable->GetMaterial()->SetCurrentTechnique(shadowMapTech);
		renderItem.Renderable->Render();
	}

	// Save ShadowMatrix
	mShadowView = mLightCamera[0]->GetViewMatrix() * mLightCamera[0]->GetProjMatrix();

	mDevice->BindFrameBuffer(currFrameBuffer);	
}

//void CascadedShadowMap::DebugDrawFrustums()
//{
//	for (int i = 0; i < mShadowTexture->GetTextureArraySize(); ++i)
//	{
//		shared_ptr<Camera> lightCamera = mLightCamera[i];
//		float4x4 viewProj = lightCamera->GetViewMatrix() * lightCamera->GetProjMatrix();
//
//		Frustumf frustum;
//		frustum.Update(viewProj);
//		frustum.UpdateCorner();
//
//		DebugDrawManager::GetSingleton().DrawFrustum(frustum, ColorRGBA::Red, false);
//	}
//}

}
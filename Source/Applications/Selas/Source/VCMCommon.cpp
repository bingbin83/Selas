
//=================================================================================================================================
// Joe Schutte
//=================================================================================================================================

#include "VCMCommon.h"

#include "Shading/SurfaceScattering.h"
#include "Shading/SurfaceParameters.h"
#include "Shading/IntegratorContexts.h"
#include "Shading/AreaLighting.h"
#include "TextureLib/Framebuffer.h"
#include "GeometryLib/Camera.h"
#include "GeometryLib/Ray.h"
#include "ThreadingLib/Thread.h"
#include "SystemLib/OSThreading.h"
#include "SystemLib/Atomic.h"
#include "SystemLib/MinMax.h"
#include "SystemLib/SystemTime.h"

#include "embree3/rtcore.h"
#include "embree3/rtcore_ray.h"

#define MaxBounceCount_         10

#define EnableMultiThreading_   1
#define IntegrationSeconds_     30.0f

#define VcmRadiusFactor_ 0.0025f
#define VcmRadiusAlpha_ 0.75f

namespace Selas
{
    namespace VCMCommon
    {
        //=========================================================================================================================
        void GenerateLightSample(GIIntegratorContext* context, float vcWeight, uint index, PathState& state)
        {
            Assert_(index < (1 << PathStateIndexBitCount_));

            // -- right now we're just generating a sample on the ibl
            float lightSampleWeight = 1.0f;

            LightEmissionSample sample;
            {
                // -- JSTODO - Sample area lights and such
                EmitIblLightSample(context, sample);
            }

            sample.emissionPdfW  *= lightSampleWeight;
            sample.directionPdfA *= lightSampleWeight;

            state.position        = sample.position;
            state.direction       = sample.direction;
            state.throughput      = sample.radiance * (1.0f / sample.emissionPdfW);
            state.dVCM            = sample.directionPdfA / sample.emissionPdfW;
            state.dVC             = sample.cosThetaLight / sample.emissionPdfW;
            state.dVM             = sample.cosThetaLight / sample.emissionPdfW * vcWeight;
            state.pathLength      = 1;
            state.isAreaMeasure   = 0; // -- this would be true for any non infinite light source.
                                       // -- false here since we only sample the ibl.
            state.index           = index;
        }

        //=========================================================================================================================
        void GenerateCameraSample(GIIntegratorContext* context, uint x, uint y, float lightPathCount, PathState& state)
        {
            const RayCastCameraSettings* __restrict camera = context->camera;

            Ray cameraRay = JitteredCameraRay(camera, &context->sampler, (float)x, (float)y);

            float cosThetaCamera = Dot(camera->forward, cameraRay.direction);
            float imagePointToCameraDistance = camera->virtualImagePlaneDistance / cosThetaCamera;
            float invSolidAngleMeasure = imagePointToCameraDistance * imagePointToCameraDistance / cosThetaCamera;
            float revCameraPdfW = (1.0f / invSolidAngleMeasure);

            state.position      = cameraRay.origin;
            state.direction     = cameraRay.direction;
            state.throughput    = float3::One_;
            state.dVCM          = lightPathCount * revCameraPdfW;
            state.dVC           = 0;
            state.dVM           = 0;
            state.pathLength    = 1;
            state.isAreaMeasure = 1;
            state.index         = y * (uint32)camera->viewportWidth + x;

            Assert_(state.index < (1 << PathStateIndexBitCount_));
        }

        //=========================================================================================================================
        float SearchRadius(float baseRadius, float radiusAlpha, float iterationIndex)
        {
            return baseRadius / Math::Powf(iterationIndex, 0.5f * (1.0f - radiusAlpha));
        }

        //=========================================================================================================================
        VCMIterationConstants CalculateIterationConstants(uint vmCount, uint vcCount, float baseRadius, float radiusAlpha,
                                                          float iterationIndex)
        {
            float vmSearchRadius = SearchRadius(baseRadius, radiusAlpha, iterationIndex);

            VCMIterationConstants constants;
            constants.vmCount           = vmCount;
            constants.vcCount           = vcCount;            
            constants.vmSearchRadius    = vmSearchRadius;
            constants.vmSearchRadiusSqr = vmSearchRadius * vmSearchRadius;
            constants.vmNormalization   = 1.0f / (Math::Pi_ * constants.vmSearchRadiusSqr * vmCount);
            constants.vmWeight          = Math::Pi_ * constants.vmSearchRadiusSqr * vmCount / vcCount;
            constants.vcWeight          = vcCount / (Math::Pi_ * constants.vmSearchRadiusSqr * vmCount);

            return constants;
        }
    }
}

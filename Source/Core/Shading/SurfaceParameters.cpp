//=================================================================================================================================
// Joe Schutte
//=================================================================================================================================

#include "SurfaceParameters.h"
#include "IntegratorContexts.h"

#include "SceneLib/SceneResource.h"
#include "SceneLib/ModelResource.h"
#include "SceneLib/GeometryCache.h"
#include "TextureLib/TextureFiltering.h"
#include "TextureLib/TextureResource.h"
#include "GeometryLib/Ray.h"
#include "GeometryLib/CoordinateSystem.h"
#include "MathLib/FloatFuncs.h"
#include "MathLib/ColorSpace.h"

#include "embree3/rtcore.h"
#include "embree3/rtcore_ray.h"

#define ForceNoMips_ true
#define EnableEWA_ true

namespace Selas
{
    //=============================================================================================================================
    float3 SampleTextureNormal(const TextureResource* texture, float2 uvs)
    {
        if(texture == nullptr)
            return float3::ZAxis_;

        if(texture->data->format != TextureResourceData::Float3) {
            Assert_(false);
            return float3::ZAxis_;
        }

        float3 sample;
        TextureFiltering::Triangle(texture->data, 0, uvs, sample);
        return 2.0f * sample - float3(1.0f);
    }

    //=============================================================================================================================
    static float SampleTextureOpacity(const TextureResource* texture, float2 uvs)
    {
        if(texture == nullptr)
            return 1.0f;

        if(texture->data->format != TextureResourceData::Float4) {
            return 1.0f;
        }

        float4 sample;
        TextureFiltering::Triangle(texture->data, 0, uvs, sample);

        return sample.w;
    }

    //=============================================================================================================================
    template <typename Type_>
    static Type_ SampleTexture(const TextureResource* texture, float2 uvs, bool sRGB, Type_ defaultValue)
    {
        if(texture == nullptr)
            return defaultValue;

        Type_ sample;
        TextureFiltering::Triangle(texture->data, 0, uvs, sample);

        if(sRGB) {
            sample = Math::SrgbToLinearPrecise(sample);
        }

        return sample;
    }

    //=============================================================================================================================
    static float SampleTextureFloat(const TextureResource* texture, float2 uvs, bool sRGB, float defaultValue)
    {
        if(texture == nullptr)
            return defaultValue;

        if(texture->data->format == TextureResourceData::Float) {
            return SampleTexture(texture, uvs, sRGB, defaultValue);
        }
        else if(texture->data->format == TextureResourceData::Float2) {
            return SampleTexture(texture, uvs, sRGB, float2(defaultValue, 0.0f)).x;
        }
        else if(texture->data->format == TextureResourceData::Float3) {
            return SampleTexture(texture, uvs, sRGB, float3(defaultValue, 0.0f, 0.0f)).x;
        }
        else if(texture->data->format == TextureResourceData::Float4) {
            return SampleTexture(texture, uvs, sRGB, float4(defaultValue, 0.0f, 0.0f, 0.0f)).x;
        }

        Assert_(false);
        return 0.0f;
    }

    //=============================================================================================================================
    static float3 SampleTextureFloat3(const TextureResource* texture, float2 uvs, bool sRGB, float3 defaultValue)
    {
        if(texture == nullptr)
            return defaultValue;

        if(texture->data->format == TextureResourceData::Float) {
            float val;
            val = SampleTexture(texture, uvs, sRGB, 0.0f);
            return float3(val, val, val);
        }
        else if(texture->data->format == TextureResourceData::Float3) {
            return SampleTexture(texture, uvs, sRGB, defaultValue);
        }
        else if(texture->data->format == TextureResourceData::Float4) {
            float4 val = SampleTexture(texture, uvs, sRGB, float4(defaultValue, 1.0f));
            return val.XYZ();
        }

        Assert_(false);
        return float3(0.0f);
    }

    //=============================================================================================================================
    static float4 SampleTextureFloat4(const TextureResource* texture, float2 uvs, bool sRGB, float defaultValue)
    {
        if(texture == nullptr)
            return float4(defaultValue, defaultValue, defaultValue, defaultValue);

        if(texture->data->format == TextureResourceData::Float) {
            float val = SampleTexture(texture, uvs, sRGB, defaultValue);
            return float4(val, val, val, 1.0f);
        }
        else if(texture->data->format == TextureResourceData::Float3) {
            float3 value = SampleTexture(texture, uvs, sRGB, float3(defaultValue, defaultValue, defaultValue));
            return float4(value, 1.0f);
        }
        else if(texture->data->format == TextureResourceData::Float4) {
            return SampleTexture(texture, uvs, sRGB, float4(defaultValue, defaultValue, defaultValue, defaultValue));
        }

        Assert_(false);
        return float4(0.0f);
    }

    //=============================================================================================================================
    bool CalculateSurfaceParams(const GIIntegratorContext* context, const HitParameters* __restrict hit,
                                SurfaceParameters& surface)
    {
        float4x4 localToWorld;
        ModelGeometryUserData* modelData;
        ModelDataFromRayIds(context->scene, hit->instId, hit->geomId, localToWorld, modelData);

        TextureCache* textureCache = context->textureCache;
        GeometryCache* geometryCache = context->geometryCache;
        const MaterialResourceData* materialResource = modelData->material;

        bool needsGeometry = modelData->flags & (HasNormals | HasTangents | HasUvs);
        if(needsGeometry) {
            context->geometryCache->EnsureSubsceneGeometryLoaded(modelData->subscene);
        }

        Align_(16) float3 normal;
        if(modelData->flags & HasNormals) {
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, &normal.x, 3);

            normal = MatrixMultiplyVector(normal, localToWorld);
        }
        else {
            normal = MatrixMultiplyVector(hit->normal, localToWorld);
        }

        float3 n = Normalize(normal);
        float3 t, b;

        if(modelData->flags & HasTangents) {
            Align_(16) float4 localTangent;
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, &localTangent.x, 4);

            t = MatrixMultiplyVector(localTangent.XYZ(), localToWorld);
            b = Cross(n, t) * localTangent.w;
        }
        else {
            MakeOrthogonalCoordinateSystem(n, &t, &b);
        }

        Align_(16) float2 uvs = float2(0.0f, 0.0f);
        if(modelData->flags & HasUvs) {
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, &uvs.x, 2);
        }

        if(needsGeometry) {
            context->geometryCache->FinishUsingSubceneGeometry(modelData->subscene);
        }

        if(materialResource->flags & eUsesPtex) {
            PtexTexture* texture = textureCache->FetchPtex(modelData->baseColorTextureHandle);

            Ptex::PtexFilter::Options opts(Ptex::PtexFilter::FilterType::f_bspline);
            Ptex::PtexFilter* filter = Ptex::PtexFilter::getFilter(texture, opts);

            float3 sample;
            filter->eval(&sample.x, 0, 3, hit->primId, hit->baryCoords.x, hit->baryCoords.y, 0, 0, 0, 0);
            surface.baseColor = Pow(sample, 2.2f);

            filter->release();
            texture->release();
        }
        else {
            const TextureResource* baseColorTexture = textureCache->FetchTexture(modelData->baseColorTextureHandle);
            surface.baseColor = SampleTextureFloat3(baseColorTexture, uvs, true, materialResource->baseColor);
            surface.baseColor = Pow(surface.baseColor, 2.2f);
            textureCache->ReleaseTexture(modelData->baseColorTextureHandle);
        }

        // -- Calculate tangent space transforms
        float3x3 tangentToWorld = MakeFloat3x3(t, n, b);

        surface.worldToTangent     = MatrixTranspose(tangentToWorld);
        surface.position           = hit->position;
        surface.error              = hit->error;
        surface.materialFlags      = materialResource->flags;
        surface.transmittanceColor = materialResource->transmittanceColor;
        surface.sheen              = materialResource->scalarAttributeValues[eSheen];
        surface.sheenTint          = materialResource->scalarAttributeValues[eSheenTint];
        surface.clearcoat          = materialResource->scalarAttributeValues[eClearcoat];
        surface.clearcoatGloss     = materialResource->scalarAttributeValues[eClearcoatGloss];
        surface.specTrans          = Saturate(materialResource->scalarAttributeValues[eSpecTrans]);
        surface.diffTrans          = materialResource->scalarAttributeValues[eDiffuseTrans] * 0.5f;
        surface.flatness           = materialResource->scalarAttributeValues[eFlatness];
        surface.anisotropic        = materialResource->scalarAttributeValues[eAnisotropic];
        surface.specularTint       = materialResource->scalarAttributeValues[eSpecularTint];
        surface.roughness          = materialResource->scalarAttributeValues[eRoughness];
        surface.metallic           = Saturate(materialResource->scalarAttributeValues[eMetallic]);
        surface.scatterDistance    = materialResource->scalarAttributeValues[eScatterDistance];
        surface.ior                = materialResource->scalarAttributeValues[eIor];
        surface.lightSetIndex      = modelData->lightSetIndex;

        surface.shader = materialResource->shader;
        surface.view = hit->view;

        // -- better way to handle this would be for the ray to know what IOR it is within
        surface.relativeIOR = ((materialResource->flags & eTransparent) && Dot(hit->view, n) < 0.0f) 
                            ? surface.ior : 1.0f / surface.ior;

        return true;
    }

    //=============================================================================================================================
    bool CalculateSurfaceParams(const GIIntegratorContext* context, const HitParameters* __restrict hit,
                                ModelGeometryUserData* modelData, float4x4 localToWorld, Ptex::PtexFilter* filter,
                                SurfaceParameters& surface)
    {
        TextureCache* textureCache = context->textureCache;
        GeometryCache* geometryCache = context->geometryCache;
        const MaterialResourceData* materialResource = modelData->material;

        bool needsGeometry = modelData->flags & (HasNormals | HasTangents | HasUvs);
        if(needsGeometry) {
            context->geometryCache->EnsureSubsceneGeometryLoaded(modelData->subscene);
        }

        Align_(16) float3 normal;
        if(modelData->flags & HasNormals) {
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, &normal.x, 3);

            normal = MatrixMultiplyVector(normal, localToWorld);
        }
        else {
            normal = MatrixMultiplyVector(hit->normal, localToWorld);
        }

        float3 n = Normalize(normal);
        float3 t, b;

        if(modelData->flags & HasTangents) {
            Align_(16) float4 localTangent;
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, &localTangent.x, 4);

            t = MatrixMultiplyVector(localTangent.XYZ(), localToWorld);
            b = Cross(n, t) * localTangent.w;
        }
        else {
            MakeOrthogonalCoordinateSystem(n, &t, &b);
        }

        Align_(16) float2 uvs = float2(0.0f, 0.0f);
        if(modelData->flags & HasUvs) {
            rtcInterpolate0(modelData->rtcGeometry, hit->primId, hit->baryCoords.x, hit->baryCoords.y,
                            RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, &uvs.x, 2);
        }

        if(needsGeometry) {
            context->geometryCache->FinishUsingSubceneGeometry(modelData->subscene);
        }

        if(materialResource->flags & eUsesPtex) {
            float3 sample;
            filter->eval(&sample.x, 0, 3, hit->primId, hit->baryCoords.x, hit->baryCoords.y, 0, 0, 0, 0);
            surface.baseColor = Pow(sample, 2.2f);
        }
        else {
            const TextureResource* baseColorTexture = textureCache->FetchTexture(modelData->baseColorTextureHandle);
            surface.baseColor = SampleTextureFloat3(baseColorTexture, uvs, true, materialResource->baseColor);
            surface.baseColor = Pow(surface.baseColor, 2.2f);
            textureCache->ReleaseTexture(modelData->baseColorTextureHandle);
        }

        // -- Calculate tangent space transforms
        float3x3 tangentToWorld = MakeFloat3x3(t, n, b);

        surface.worldToTangent     = MatrixTranspose(tangentToWorld);
        surface.position           = hit->position;
        surface.error              = hit->error;
        surface.materialFlags      = materialResource->flags;
        surface.transmittanceColor = materialResource->transmittanceColor;
        surface.sheen              = materialResource->scalarAttributeValues[eSheen];
        surface.sheenTint          = materialResource->scalarAttributeValues[eSheenTint];
        surface.clearcoat          = materialResource->scalarAttributeValues[eClearcoat];
        surface.clearcoatGloss     = materialResource->scalarAttributeValues[eClearcoatGloss];
        surface.specTrans          = Saturate(materialResource->scalarAttributeValues[eSpecTrans]);
        surface.diffTrans          = materialResource->scalarAttributeValues[eDiffuseTrans] * 0.5f;
        surface.flatness           = materialResource->scalarAttributeValues[eFlatness];
        surface.anisotropic        = materialResource->scalarAttributeValues[eAnisotropic];
        surface.specularTint       = materialResource->scalarAttributeValues[eSpecularTint];
        surface.roughness          = materialResource->scalarAttributeValues[eRoughness];
        surface.metallic           = Saturate(materialResource->scalarAttributeValues[eMetallic]);
        surface.scatterDistance    = materialResource->scalarAttributeValues[eScatterDistance];
        surface.ior                = materialResource->scalarAttributeValues[eIor];
        surface.lightSetIndex      = modelData->lightSetIndex;

        surface.shader = materialResource->shader;
        surface.view = hit->view;

        // -- better way to handle this would be for the ray to know what IOR it is within
        surface.relativeIOR = ((materialResource->flags & eTransparent) && Dot(hit->view, n) < 0.0f) 
                            ? surface.ior : 1.0f / surface.ior;

        return true;
    }

    //=============================================================================================================================
    bool CalculatePassesAlphaTest(const ModelGeometryUserData* geomData, uint32 geomId, uint32 primId, float2 baryCoords)
    {
        // JSTODO - Need access to the cache
        return true;

        //const Material* material = geomData->material;
        //Assert_(material->resource->flags & MaterialFlags::eAlphaTested);

        //Align_(16) float2 uvs = float2::Zero_;
        //if(geomData->flags & HasUvs) {
        //    rtcInterpolate0(geomData->rtcGeometry, primId, baryCoords.x, baryCoords.y,
        //                    RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, &uvs.x, 2);
        //}

        //static const float kAlphaTestCutoff = 0.5f;
        //return SampleTextureOpacity(nullptr, uvs, material->baseColorTextureIndex) > kAlphaTestCutoff;
    }

    //=============================================================================================================================
    float CalculateDisplacement(const ModelGeometryUserData* userData, RTCGeometry rtcGeometry, uint32 primId, float2 barys)
    {
        // JSTODO - Need access to the cache
        return 0.0f;

        //Align_(16) float2 uvs = float2::Zero_;
        //if(userData->flags & HasUvs) {
        //    rtcInterpolate0(rtcGeometry, primId, barys.x, barys.y, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, &uvs.x, 2);
        //}

        //const MaterialResource* material = userData->material;

        //float displacement = SampleTextureFloat(nullptr, uvs, material->scalarAttributeTextureIndices[eDisplacement], false, 0.0f);
        //if(material->flags & eInvertDisplacement) {
        //    displacement = 1.0f - displacement;
        //}

        //return material->scalarAttributeValues[eDisplacement] * displacement;
    }

    //=============================================================================================================================
    float3 GeometricTangent(const SurfaceParameters& surface)
    {
        return float3(surface.worldToTangent.r0.x, surface.worldToTangent.r1.x, surface.worldToTangent.r2.x);
    }

    //=============================================================================================================================
    float3 GeometricNormal(const SurfaceParameters& surface)
    {
        return float3(surface.worldToTangent.r0.y, surface.worldToTangent.r1.y, surface.worldToTangent.r2.y);
    }

    //=============================================================================================================================
    float3 GeometricBitangent(const SurfaceParameters& surface)
    {
        return float3(surface.worldToTangent.r0.z, surface.worldToTangent.r1.z, surface.worldToTangent.r2.z);
    }

    //=============================================================================================================================
    float3 OffsetRayOrigin(const SurfaceParameters& surface, float3 direction, float biasScale)
    {
        float directionOffset = Dot(direction, GeometricNormal(surface)) < 0.0f ? -1.0f : 1.0f;
        float3 offset = directionOffset * surface.error * biasScale * GeometricNormal(surface);
        return surface.position + offset;
    }

    //=============================================================================================================================
    float3 OffsetRayOrigin(const SurfaceParameters& surface, float3 direction, float biasScale, float& signedBiasDistance)
    {
        float directionOffset = Dot(direction, GeometricNormal(surface)) < 0.0f ? -1.0f : 1.0f;
        signedBiasDistance = directionOffset * surface.error * biasScale;
        float3 offset = signedBiasDistance * GeometricNormal(surface);
        return surface.position + offset;
    }

    //=============================================================================================================================
    float ContinuationProbability(const SurfaceParameters& surface)
    {
        float3 value = surface.baseColor;
        return Saturate(Max(Max(value.x, value.y), value.z));
    }
}

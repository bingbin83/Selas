
//=================================================================================================================================
// Joe Schutte
//=================================================================================================================================

#include "Shading/Disney.h"
#include "Shading/SurfaceScattering.h"
#include "Shading/SurfaceParameters.h"
#include "Shading/IntegratorContexts.h"

#include "Shading/Fresnel.h"
#include "Shading/Ggx.h"
#include "MathLib/FloatFuncs.h"
#include "MathLib/Trigonometric.h"
#include "MathLib/Projection.h"
#include "SystemLib/MinMax.h"

namespace Selas
{
    using namespace Math;

    // There a lot going on here so I wrote a blog post about it.
    // https://schuttejoe.github.io/post/DisneyBsdf/

    //=============================================================================================================================
    static void CalculateLobePdfs(const SurfaceParameters& surface,
                                  float& pSpecular, float& pDiffuse, float& pClearcoat, float& pSpecTrans)
    {
        float metallicBRDF   = surface.metallic;
        float specularBSDF   = (1.0f - surface.metallic) * surface.specTrans;
        float dielectricBRDF = (1.0f - surface.specTrans) * (1.0f - surface.metallic);

        float specularWeight     = metallicBRDF + dielectricBRDF;
        float transmissionWeight = specularBSDF;
        float diffuseWeight      = dielectricBRDF;
        float clearcoatWeight    = 1.0f * Saturate(surface.clearcoat); 

        float norm = 1.0f / (specularWeight + transmissionWeight + diffuseWeight + clearcoatWeight);

        pSpecular  = specularWeight     * norm;
        pSpecTrans = transmissionWeight * norm;
        pDiffuse   = diffuseWeight      * norm;
        pClearcoat = clearcoatWeight    * norm;
    }

    //=============================================================================================================================
    static float ThinTransmissionRoughness(float ior, float roughness)
    {
        // -- Disney scales by (.65 * eta - .35) based on figure 15 of the 2015 PBR course notes. Based on their figure the results
        // -- match a geometrically thin solid fairly well but it is odd to me that roughness is decreased until an IOR of just
        // -- over 2.
        return Saturate((0.65f * ior - 0.35f) * roughness);
    }

    //=============================================================================================================================
    static void CalculateAnisotropicParams(float roughness, float anisotropic, float& ax, float& ay)
    {
        float aspect = Sqrtf(1.0f - 0.9f * anisotropic);
        ax = Max(0.001f, Square(roughness) / aspect);
        ay = Max(0.001f, Square(roughness) * aspect);
    }

    //=============================================================================================================================
    static float3 CalculateTint(float3 baseColor)
    {
        // -- The color tint is never mentioned in the SIGGRAPH presentations as far as I recall but it was done in the BRDF
        // -- Explorer so I'll replicate that here.
        float luminance = Dot(float3(0.3f, 0.6f, 1.0f), baseColor);
        return (luminance > 0.0f) ? baseColor * (1.0f / luminance) : float3::One_;
    }

    //=============================================================================================================================
    // -- "generalized" Trowbridge-Reitz curve ungeneralized with a hard-coded exponent of 1
    static float GTR1(float absDotHL, float a)
    {
        if(a >= 1) {
            return InvPi_;
        }

        float a2 = a * a;
        return (a2 - 1.0f) / (Pi_ * Log2(a2) * (1.0f + (a2 - 1.0f) * absDotHL * absDotHL));
    }

    //=============================================================================================================================
    static float EvaluateDisneyClearcoat(float clearcoat, float alpha, const float3& wo, const float3& wm, const float3& wi,
                                         float& fPdfW, float& rPdfW)
    {
        if(clearcoat <= 0.0f) {
            return 0.0f;
        }

        float absDotNH = AbsCosTheta(wm);
        float absDotNL = AbsCosTheta(wi);
        float absDotNV = AbsCosTheta(wo);
        float dotHL = Dot(wm, wi);

        float d = GTR1(absDotNH, Lerp(0.1f, 0.001f, alpha));
        float f = Fresnel::Schlick(0.04f, dotHL);
        float gl = Bsdf::SeparableSmithGGXG1(wi, 0.25f);
        float gv = Bsdf::SeparableSmithGGXG1(wo, 0.25f);

        fPdfW = d / (4.0f * AbsDot(wo, wm));
        rPdfW = d / (4.0f * AbsDot(wi, wm));

        return 0.25f * clearcoat * d * f * gl * gv;
    }

    //=============================================================================================================================
    static float3 EvaluateSheen(const SurfaceParameters& surface, const float3& wo, const float3& wm, const float3& wi)
    {
        if(surface.sheen <= 0.0f) {
            return float3::Zero_;
        }

        float dotHL = Absf(Dot(wm, wi));

        float3 tint = CalculateTint(surface.baseColor);
        return surface.sheen * Lerp(float3(1.0f), tint, surface.sheenTint) * Fresnel::SchlickWeight(dotHL);
    }

    //=============================================================================================================================
    static float3 DisneyFresnel(const SurfaceParameters& surface, const float3& wo, const float3& wm, const float3& wi)
    {
        float dotHV = Dot(wm, wo);

        float3 tint = CalculateTint(surface.baseColor);

        // -- See section 3.1 and 3.2 of the 2015 PBR presentation + the Disney BRDF explorer (which does their 2012 remapping
        // -- rather than the SchlickR0FromRelativeIOR seen here but they mentioned the switch in 3.2).
        float3 R0 = Fresnel::SchlickR0FromRelativeIOR(surface.relativeIOR) * Lerp(float3(1.0f), tint, surface.specularTint);
               R0 = Lerp(R0, surface.baseColor, surface.metallic);

        float dielectricFresnel = Fresnel::Dielectric(dotHV, 1.0f, surface.ior);
        float3 metallicFresnel = Fresnel::Schlick(R0, Dot(wi, wm));

        return Lerp(float3(dielectricFresnel), metallicFresnel, surface.metallic);
    }

    //=============================================================================================================================
    static float3 EvaluateDisneyBRDF(const SurfaceParameters& surface, const float3& wo, const float3& wm, const float3& wi,
                                     float& fPdf, float& rPdf)
    {
        fPdf = 0.0f;
        rPdf = 0.0f;

        float dotNL = CosTheta(wi);
        float dotNV = CosTheta(wo);
        if(dotNL <= 0.0f || dotNV <= 0.0f) {
            return float3::Zero_;
        }

        float ax, ay;
        CalculateAnisotropicParams(surface.roughness, surface.anisotropic, ax, ay);

        float d = Bsdf::GgxAnisotropicD(wm, ax, ay);
        float gl = Bsdf::SeparableSmithGGXG1(wi, wm, ax, ay);
        float gv = Bsdf::SeparableSmithGGXG1(wo, wm, ax, ay);

        float3 f = DisneyFresnel(surface, wo, wm, wi);

        Bsdf::GgxVndfAnisotropicPdf(wi, wm, wo, ax, ay, fPdf, rPdf);
        fPdf *= (1.0f / (4 * AbsDot(wo, wm)));
        rPdf *= (1.0f / (4 * AbsDot(wi, wm)));

        return d * gl * gv * f / (4.0f * dotNL * dotNV);
    }

    //=============================================================================================================================
    static bool SampleDisneyBRDF(CSampler* sampler, const SurfaceParameters& surface, float3 v, BsdfSample& sample)
    {
        float3 wo = Normalize(MatrixMultiply(v, surface.worldToTangent));

        // -- Calculate Anisotropic params
        float ax, ay;
        CalculateAnisotropicParams(surface.roughness, surface.anisotropic, ax, ay);

        // -- Sample visible distribution of normals
        float r0 = sampler->UniformFloat();
        float r1 = sampler->UniformFloat();
        float3 wm = Bsdf::SampleGgxVndfAnisotropic(wo, ax, ay, r0, r1);

        // -- Reflect over wm
        float3 wi = Normalize(Reflect(wm, wo));
        if(CosTheta(wi) <= 0.0f) {
            sample.forwardPdfW = 0.0f;
            sample.reversePdfW = 0.0f;
            sample.reflectance = float3::Zero_;
            sample.wi = float3::Zero_;
            return false;
        }

        // -- Fresnel term for this lobe is complicated since we're blending with both the metallic and the specularTint
        // -- parameters plus we must take the IOR into account for dielectrics
        float3 F = DisneyFresnel(surface, wo, wm, wi);

        // -- Since we're sampling the distribution of visible normals the pdf cancels out with a number of other terms.
        // -- We are left with the weight G2(wi, wo, wm) / G1(wi, wm) and since Disney uses a separable masking function
        // -- we get G1(wi, wm) * G1(wo, wm) / G1(wi, wm) = G1(wo, wm) as our weight.
        float G1v = Bsdf::SeparableSmithGGXG1(wo, wm, ax, ay);
        float3 specular = G1v * F;

        sample.flags = SurfaceEventFlags::eScatterEvent;
        sample.reflectance = specular;
        sample.wi = Normalize(MatrixMultiply(wi, MatrixTranspose(surface.worldToTangent)));
        Bsdf::GgxVndfAnisotropicPdf(wi, wm, wo, ax, ay, sample.forwardPdfW, sample.reversePdfW);

        sample.forwardPdfW *= (1.0f / (4 * AbsDot(wo, wm)));
        sample.reversePdfW *= (1.0f / (4 * AbsDot(wi, wm)));

        return true;
    }

    //=============================================================================================================================
    static float3 EvaluateDisneySpecTransmission(const SurfaceParameters& surface, const float3& wo, const float3& wm,
                                                 const float3& wi, float ax, float ay, bool thin)
    {
        float relativeIor = surface.relativeIOR;
        float n2 = relativeIor * relativeIor;

        float absDotNL = AbsCosTheta(wi);
        float absDotNV = AbsCosTheta(wo);
        float dotHL = Dot(wm, wi);
        float dotHV = Dot(wm, wo);
        float absDotHL = Absf(dotHL);
        float absDotHV = Absf(dotHV);

        float d = Bsdf::GgxAnisotropicD(wm, ax, ay);
        float gl = Bsdf::SeparableSmithGGXG1(wi, wm, ax, ay);
        float gv = Bsdf::SeparableSmithGGXG1(wo, wm, ax, ay);

        float f = Fresnel::Dielectric(dotHV, 1.0f, surface.ior);

        float3 color;
        if(thin)
            color = Sqrt(surface.baseColor);
        else
            color = surface.baseColor;

        // Note that we are intentionally leaving out the 1/n2 spreading factor since for VCM we will be evaluating particles with
        // this. That means we'll need to model the air-[other medium] transmission if we ever place the camera inside a non-air
        // medium.
        float c = (absDotHL * absDotHV) / (absDotNL * absDotNV);
        float t = (n2 / Square(dotHL + relativeIor * dotHV));
        return color * c * t * (1.0f - f) * gl * gv * d;
    }

    //=============================================================================================================================
    static float EvaluateDisneyRetroDiffuse(const SurfaceParameters& surface, const float3& wo, const float3& wm, const float3& wi)
    {
        float dotNL = AbsCosTheta(wi);
        float dotNV = AbsCosTheta(wo);

        float roughness = surface.roughness * surface.roughness;

        float rr = 0.5f + 2.0f * dotNL * dotNL * roughness;
        float fl = Fresnel::SchlickWeight(dotNL);
        float fv = Fresnel::SchlickWeight(dotNV);
        
        return rr * (fl + fv + fl * fv * (rr - 1.0f));
    }

    //=============================================================================================================================
    static float EvaluateDisneyDiffuse(const SurfaceParameters& surface, const float3& wo, const float3& wm, const float3& wi,
                                       bool thin)
    {
        float dotNL = AbsCosTheta(wi);
        float dotNV = AbsCosTheta(wo);

        float fl = Fresnel::SchlickWeight(dotNL);
        float fv = Fresnel::SchlickWeight(dotNV);

        float hanrahanKrueger = 0.0f;

        if(thin && surface.flatness > 0.0f) {
            float roughness = surface.roughness * surface.roughness;

            float dotHL = Dot(wm, wi);
            float fss90 = dotHL * dotHL * roughness;
            float fss = Lerp(1.0f, fss90, fl) * Lerp(1.0f, fss90, fv);

            float ss = 1.25f * (fss * (1.0f / (dotNL + dotNV) - 0.5f) + 0.5f);
            hanrahanKrueger = ss;
        }

        float lambert = 1.0f;
        float retro = EvaluateDisneyRetroDiffuse(surface, wo, wm, wi);
        float subsurfaceApprox = Lerp(lambert, hanrahanKrueger, thin ? surface.flatness : 0.0f);

        return InvPi_ * (retro + subsurfaceApprox * (1.0f - 0.5f * fl) * (1.0f - 0.5f * fv));
    }

    //=============================================================================================================================
    static bool SampleDisneyClearcoat(CSampler* sampler, const SurfaceParameters& surface, const float3& v, BsdfSample& sample)
    {
        float3 wo = Normalize(MatrixMultiply(v, surface.worldToTangent));

        float a = 0.25f;
        float a2 = a * a;

        float r0 = sampler->UniformFloat();
        float r1 = sampler->UniformFloat();
        float cosTheta = Sqrtf(Max<float>(0, (1.0f - Powf(a2, 1.0f - r0)) / (1.0f - a2)));
        float sinTheta = Sqrtf(Max<float>(0, 1.0f - cosTheta * cosTheta));
        float phi = TwoPi_ * r1;

        float3 wm = float3(sinTheta * Cosf(phi), cosTheta, sinTheta * Sinf(phi));
        if(Dot(wm, wo) < 0.0f) {
            wm = -wm;
        }

        float3 wi = Reflect(wm, wo);
        if(Dot(wi, wo) < 0.0f) {
            return false;
        }

        float clearcoatWeight = surface.clearcoat;
        float clearcoatGloss = surface.clearcoatGloss;

        float dotNH = CosTheta(wm);
        float dotLH = Dot(wm, wi);

        float d = GTR1(Absf(dotNH), Lerp(0.1f, 0.001f, clearcoatGloss));
        float f = Fresnel::Schlick(0.04f, dotLH);
        float g = Bsdf::SeparableSmithGGXG1(wi, 0.25f) * Bsdf::SeparableSmithGGXG1(wo, 0.25f);

        float fPdf = d / (4.0f * Dot(wo, wm));

        sample.reflectance = float3(0.25f * clearcoatWeight * g * f * d) / fPdf;
        sample.wi = Normalize(MatrixMultiply(wi, MatrixTranspose(surface.worldToTangent)));
        sample.forwardPdfW = fPdf;
        sample.reversePdfW = d / (4.0f * Dot(wi, wm));

        return true;
    }

    //=============================================================================================================================
    static float3 CalculateExtinction(float3 apparantColor, float scatterDistance)
    {
        float3 a = apparantColor;
        float3 s = float3(1.9f) - a + 3.5f * (a - float3(0.8f)) * (a - float3(0.8f));

        return 1.0f / (s * scatterDistance);
    }

    //=============================================================================================================================
    static bool SampleDisneySpecTransmission(CSampler* sampler, const SurfaceParameters& surface, float3 v, bool thin,
                                             BsdfSample& sample)
    {
        float3 wo = MatrixMultiply(v, surface.worldToTangent);
        if(CosTheta(wo) == 0.0) {
            sample.forwardPdfW = 0.0f;
            sample.reversePdfW = 0.0f;
            sample.reflectance = float3::Zero_;
            sample.wi = float3::Zero_;
            return false;
        }

        // -- Scale roughness based on IOR
        float rscaled = thin ? ThinTransmissionRoughness(surface.ior, surface.roughness) : surface.roughness;
         
        float tax, tay;
        CalculateAnisotropicParams(rscaled, surface.anisotropic, tax, tay);
        
        // -- Sample visible distribution of normals
        float r0 = sampler->UniformFloat();
        float r1 = sampler->UniformFloat();
        float3 wm = Bsdf::SampleGgxVndfAnisotropic(wo, tax, tay, r0, r1);

        float dotVH = Dot(wo, wm);
        if(wm.y < 0.0f) {
            dotVH = -dotVH;
        }

        float ni = wo.y > 0.0f ? 1.0f : surface.ior;
        float nt = wo.y > 0.0f ? surface.ior : 1.0f;
        float relativeIOR = ni / nt;

        // -- Disney uses the full dielectric Fresnel equation for transmission. We also importance sample F to switch between
        // -- refraction and reflection at glancing angles.
        float F = Fresnel::Dielectric(dotVH, 1.0f, surface.ior);
        
        // -- Since we're sampling the distribution of visible normals the pdf cancels out with a number of other terms.
        // -- We are left with the weight G2(wi, wo, wm) / G1(wi, wm) and since Disney uses a separable masking function
        // -- we get G1(wi, wm) * G1(wo, wm) / G1(wi, wm) = G1(wo, wm) as our weight.
        float G1v = Bsdf::SeparableSmithGGXG1(wo, wm, tax, tay);

        float pdf;

        float3 wi;
        if(sampler->UniformFloat() <= F) {
            wi = Normalize(Reflect(wm, wo));

            sample.flags = SurfaceEventFlags::eScatterEvent;
            sample.reflectance = G1v * surface.baseColor;

            float jacobian = (4 * AbsDot(wo, wm));
            pdf = F / jacobian;
        }
        else {
            if(thin) {
                // -- When the surface is thin so it refracts into and then out of the surface during this shading event.
                // -- So the ray is just reflected then flipped and we use the sqrt of the surface color.
                wi = Reflect(wm, wo);
                wi.y = -wi.y;
                sample.reflectance = G1v * Sqrt(surface.baseColor);

                // -- Since this is a thin surface we are not ending up inside of a volume so we treat this as a scatter event.
                sample.flags = SurfaceEventFlags::eScatterEvent;
            }
            else {
                if(Transmit(wm, wo, relativeIOR, wi)) {
                    sample.flags = SurfaceEventFlags::eTransmissionEvent;
                    sample.medium.phaseFunction = dotVH > 0.0f ? MediumPhaseFunction::eIsotropic : MediumPhaseFunction::eVacuum;
                    sample.medium.extinction = CalculateExtinction(surface.transmittanceColor, surface.scatterDistance);
                }
                else {
                    sample.flags = SurfaceEventFlags::eScatterEvent;
                    wi = Reflect(wm, wo);
                }

                sample.reflectance = G1v * surface.baseColor;
            }

            wi = Normalize(wi);
            
            float dotLH = Absf(Dot(wi, wm));
            float jacobian = dotLH / (Square(dotLH + surface.relativeIOR * dotVH));
            pdf = (1.0f - F) / jacobian;
        }

        if(CosTheta(wi) == 0.0f) {
            sample.forwardPdfW = 0.0f;
            sample.reversePdfW = 0.0f;
            sample.reflectance = float3::Zero_;
            sample.wi = float3::Zero_;
            return false;
        }

        if(surface.roughness < 0.01f) {
            sample.flags |= SurfaceEventFlags::eDiracEvent;
        }

        // -- calculate pdf terms
        Bsdf::GgxVndfAnisotropicPdf(wi, wm, wo, tax, tay, sample.forwardPdfW, sample.reversePdfW);
        sample.forwardPdfW *= pdf;
        sample.reversePdfW *= pdf;

        // -- convert wi back to world space
        sample.wi = Normalize(MatrixMultiply(wi, MatrixTranspose(surface.worldToTangent)));

        return true;
    }

    //=============================================================================================================================
    static float3 SampleCosineWeightedHemisphere(float r0, float r1)
    {
        float r = Sqrtf(r0);
        float theta = TwoPi_ * r1;

        return float3(r * Cosf(theta), Sqrtf(Max(0.0f, 1 - r0)), r * Sinf(theta));
    }

    //=============================================================================================================================
    static bool SampleDisneyDiffuse(CSampler* sampler, const SurfaceParameters& surface, float3 v, bool thin, BsdfSample& sample)
    {
        float3 wo = MatrixMultiply(v, surface.worldToTangent);

        float sign = Sign(CosTheta(wo));

        // -- Sample cosine lobe
        float r0 = sampler->UniformFloat();
        float r1 = sampler->UniformFloat();
        float3 wi = sign * SampleCosineWeightedHemisphere(r0, r1);
        float3 wm = Normalize(wi + wo);

        float dotNL = CosTheta(wi);
        if(dotNL == 0.0f) {
            sample.forwardPdfW = 0.0f;
            sample.reversePdfW = 0.0f;
            sample.reflectance = float3::Zero_;
            sample.wi = float3::Zero_;
            return false;
        }

        float dotNV = CosTheta(wo);

        float pdf;

        SurfaceEventFlags eventType = SurfaceEventFlags::eScatterEvent;

        float3 color = surface.baseColor;

        float p = sampler->UniformFloat();
        if(p <= surface.diffTrans) {
            wi = -wi;
            pdf = surface.diffTrans;

            if(thin)
                color = Sqrt(color);
            else {
                eventType = SurfaceEventFlags::eTransmissionEvent;

                sample.medium.phaseFunction = MediumPhaseFunction::eIsotropic;
                sample.medium.extinction = CalculateExtinction(surface.transmittanceColor, surface.scatterDistance);
            }
        }
        else {
            pdf = (1.0f - surface.diffTrans);
        }

        float3 sheen = EvaluateSheen(surface, wo, wm, wi);

        float diffuse = EvaluateDisneyDiffuse(surface, wo, wm, wi, thin);

        Assert_(pdf > 0.0f);
        sample.reflectance = sheen + color * (diffuse / pdf);
        sample.wi = Normalize(MatrixMultiply(wi, MatrixTranspose(surface.worldToTangent)));
        sample.forwardPdfW = Absf(dotNL) * pdf;
        sample.reversePdfW = Absf(dotNV) * pdf;
        sample.flags = eventType;
        return true;
    }

    //=============================================================================================================================
    float3 EvaluateDisney(const SurfaceParameters& surface, float3 v, float3 l, bool thin, float& forwardPdf, float& reversePdf)
    {
        float3 wo = Normalize(MatrixMultiply(v, surface.worldToTangent));
        float3 wi = Normalize(MatrixMultiply(l, surface.worldToTangent));
        float3 wm = Normalize(wo + wi);

        float dotNV = CosTheta(wo);
        float dotNL = CosTheta(wi);

        float3 reflectance = float3::Zero_;
        forwardPdf = 0.0f;
        reversePdf = 0.0f;

        float pBRDF, pDiffuse, pClearcoat, pSpecTrans;
        CalculateLobePdfs(surface, pBRDF, pDiffuse, pClearcoat, pSpecTrans);

        float metallic = surface.metallic;
        float specTrans = surface.specTrans;

        // calculate all of the anisotropic params
        float ax, ay;
        CalculateAnisotropicParams(surface.roughness, surface.anisotropic, ax, ay);

        float diffuseWeight = (1.0f - metallic) * (1.0f - specTrans);
        float transWeight   = (1.0f - metallic) * specTrans;

        // -- Clearcoat
        bool upperHemisphere = dotNL > 0.0f && dotNV > 0.0f;
        if(upperHemisphere && surface.clearcoat > 0.0f) {
            
            float forwardClearcoatPdfW;
            float reverseClearcoatPdfW;

            float clearcoat = EvaluateDisneyClearcoat(surface.clearcoat, surface.clearcoatGloss, wo, wm, wi,
                                                      forwardClearcoatPdfW, reverseClearcoatPdfW);
            reflectance += float3(clearcoat);
            forwardPdf += pClearcoat * forwardClearcoatPdfW;
            reversePdf += pClearcoat * reverseClearcoatPdfW;
        }

        // -- Diffuse
        if(diffuseWeight > 0.0f) {
            float forwardDiffusePdfW = AbsCosTheta(wi);
            float reverseDiffusePdfW = AbsCosTheta(wo);
            float diffuse = EvaluateDisneyDiffuse(surface, wo, wm, wi, thin);

            float3 sheen = EvaluateSheen(surface, wo, wm, wi);

            reflectance += diffuseWeight * (diffuse * surface.baseColor + sheen);

            forwardPdf += pDiffuse * forwardDiffusePdfW;
            reversePdf += pDiffuse * reverseDiffusePdfW;
        }

        // -- transmission
        if(transWeight > 0.0f) {

            // Scale roughness based on IOR (Burley 2015, Figure 15).
            float rscaled = thin ? ThinTransmissionRoughness(surface.ior, surface.roughness) : surface.roughness;
            float tax, tay;
            CalculateAnisotropicParams(rscaled, surface.anisotropic, tax, tay);

            float3 transmission = EvaluateDisneySpecTransmission(surface, wo, wm, wi, tax, tay, thin);
            reflectance += transWeight * transmission;

            float forwardTransmissivePdfW;
            float reverseTransmissivePdfW;
            Bsdf::GgxVndfAnisotropicPdf(wi, wm, wo, tax, tay, forwardTransmissivePdfW, reverseTransmissivePdfW);

            float dotLH = Dot(wm, wi);
            float dotVH = Dot(wm, wo);
            forwardPdf += pSpecTrans * forwardTransmissivePdfW / (Square(dotLH + surface.relativeIOR * dotVH));
            reversePdf += pSpecTrans * reverseTransmissivePdfW / (Square(dotVH + surface.relativeIOR * dotLH));
        }

        // -- specular
        if(upperHemisphere) {
            float forwardMetallicPdfW;
            float reverseMetallicPdfW;
            float3 specular = EvaluateDisneyBRDF(surface, wo, wm, wi, forwardMetallicPdfW, reverseMetallicPdfW);

            reflectance += specular;
            forwardPdf += pBRDF * forwardMetallicPdfW / (4 * AbsDot(wo, wm));
            reversePdf += pBRDF * reverseMetallicPdfW / (4 * AbsDot(wi, wm));
        }

        reflectance = reflectance * Absf(dotNL);

        return reflectance;
    }

    //=============================================================================================================================
    bool SampleDisney(CSampler* sampler, const SurfaceParameters& surface, float3 v, bool thin, BsdfSample& sample)
    {
        float pSpecular;
        float pDiffuse;
        float pClearcoat;
        float pTransmission;
        CalculateLobePdfs(surface, pSpecular, pDiffuse, pClearcoat, pTransmission);

        bool success = false;

        float pLobe = 0.0f;
        float p = sampler->UniformFloat();
        if(p <= pSpecular) {
            success = SampleDisneyBRDF(sampler, surface, v, sample);
            pLobe = pSpecular;
        }
        else if(p > pSpecular && p <= (pSpecular + pClearcoat)) {
            success = SampleDisneyClearcoat(sampler, surface, v, sample);
            pLobe = pClearcoat;
        }
        else if(p > pSpecular + pClearcoat && p <= (pSpecular + pClearcoat + pDiffuse)) {
            success = SampleDisneyDiffuse(sampler, surface, v, thin, sample);
            pLobe = pDiffuse;
        }
        else if(pTransmission >= 0.0f) {
            success = SampleDisneySpecTransmission(sampler, surface, v, thin, sample);
            pLobe = pTransmission;
        }
        else {
            // -- Make sure we notice if this is occurring.
            sample.reflectance = float3(1000000.0f, 0.0f, 0.0f);
            sample.forwardPdfW = 0.000000001f;
            sample.reversePdfW = 0.000000001f;
        }

        if(pLobe > 0.0f) {
            sample.reflectance = sample.reflectance * (1.0f / pLobe);
            sample.forwardPdfW *= pLobe;
            sample.reversePdfW *= pLobe;
        }

        return success;
    }
}

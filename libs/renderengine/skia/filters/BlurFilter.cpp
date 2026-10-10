/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include "BlurFilter.h"
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkImageFilters.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <common/trace.h>
#include <log/log.h>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

const SkString kEffectSource_BlurFilter_MixEffect(R"(
    uniform shader blurredInput;
    uniform shader originalInput;
    uniform float mixFactor;

    half4 main(float2 xy) {
        return half4(mix(originalInput.eval(xy), blurredInput.eval(xy), mixFactor)).rgb1;
    }
)");

// Variable-radius separable Gaussian, following miuix-blur's progressive shader.
// Copyright 2026, compose-miuix-ui contributors (Apache-2.0).
// Adapted for the compositor: full-resolution source, bounded sampling work and no CPU readback.
const SkString kEffectSource_BlurFilter_ProgressiveEffect(R"(
    uniform shader child;
    uniform float2 sampleStep;
    uniform float2 origin;
    uniform float3 gradient;
    uniform float maxSigma;

    half4 main(float2 xy) {
        float raw = clamp(dot(gradient.xy, xy + origin) + gradient.z, 0.0, 1.0);
        float sigma = maxSigma * min(3.0 * raw * raw * (3.0 - 2.0 * raw), 1.0);
        half4 color = child.eval(xy);
        if (sigma < 0.1) return color;

        // The sharp-end kernel is bounded to sigma <= 8, so every source pixel is
        // sampled out to three sigma. Larger radii use Skia's prefiltered Gaussian.
        float inv2s2 = -0.5 / (sigma * sigma);
        float total = 1.0;
        for (int i = 1; i <= 24; i++) {
            float d = float(i);
            float w = exp(d * d * inv2s2);
            float2 offset = sampleStep * d;
            color += (child.eval(xy + offset) + child.eval(xy - offset)) * half(w);
            total += 2.0 * w;
        }
        return color / half(total);
    }
)");

// Like miuix-blur's progressive stack, blend Gaussian scales per pixel, with a
// variable-radius full-resolution endpoint instead of fading crisp edges over a blur.
const SkString kEffectSource_BlurFilter_ProgressiveCompositeEffect(R"(
    uniform shader sharp;
    uniform shader weak;
    uniform shader medium;
    uniform shader strong;
    uniform float2 origin;
    uniform float3 gradient;

    half4 main(float2 xy) {
        float raw = clamp(dot(gradient.xy, xy + origin) + gradient.z, 0.0, 1.0);
        float p = 3.0 * raw * raw * (3.0 - 2.0 * raw);
        if (p < 1.0) return mix(sharp.eval(xy), weak.eval(xy), smoothstep(0.75, 1.0, p));
        if (p < 2.0) return mix(weak.eval(xy), medium.eval(xy), smoothstep(1.0, 2.0, p));
        return mix(medium.eval(xy), strong.eval(xy), smoothstep(2.0, 3.0, p));
    }
)");

static SkMatrix getShaderTransform(const SkCanvas* canvas, const SkRect& blurRect,
                                   const float scale, const float zoomScale) {
    // 1. Apply the blur shader matrix, which scales up the blurred surface to its real size
    auto matrix = SkMatrix::Scale(scale, scale);
    // 2. Since the blurred surface has the size of the layer, we align it with the
    // top left corner of the layer position.
    matrix.postConcat(SkMatrix::Translate(blurRect.fLeft, blurRect.fTop));
    // 3. Apply the "zoom" effect as an extra scale + translate around the center of the blur.
    if (zoomScale != 1.0f) {
        matrix.postScale(zoomScale, zoomScale);
        matrix.postTranslate(
                blurRect.width() * (1 - zoomScale) / 2.0f,
                blurRect.height() * (1 - zoomScale) / 2.0f);
    }
    // 4. Finally, apply the inverse canvas matrix. The snapshot made in the BlurFilter is in the
    // original surface orientation. The inverse matrix has to be applied to align the blur
    // surface with the current orientation/position of the canvas.
    SkMatrix drawInverse;
    if (canvas != nullptr && canvas->getTotalMatrix().invert(&drawInverse)) {
        matrix.postConcat(drawInverse);
    }
    return matrix;
}

BlurFilter::BlurFilter(RuntimeEffectManager& effectManager, const float maxCrossFadeRadius)
      : mMaxCrossFadeRadius(maxCrossFadeRadius),
        mMixEffect(effectManager.mKnownEffects[kBlurFilter_MixEffect]),
        mProgressiveEffect(effectManager.mKnownEffects[kBlurFilter_ProgressiveEffect]),
        mProgressiveCompositeEffect(effectManager.mKnownEffects[kBlurFilter_ProgressiveCompositeEffect]) {}

float BlurFilter::getMaxCrossFadeRadius() const {
    return mMaxCrossFadeRadius;
}

void BlurFilter::drawProgressiveBlur(SkiaGpuContext* context, SkCanvas* canvas,
                                     const SkRRect& effectRegion, uint32_t radius, float alpha,
                                     const sk_sp<SkImage>& input) {
    SFTRACE_CALL();
    if (!input || radius == 0 || alpha <= 0 || effectRegion.isEmpty()) return;

    SkMatrix inverse;
    if (!canvas->getTotalMatrix().invert(&inverse)) return;
    const auto& bounds = effectRegion.rect();
    const float sigma = std::min(radius, 256u) * 0.45f;
    SkRect workRect = canvas->getTotalMatrix().mapRect(bounds);
    if (!workRect.intersect(SkRect::Make(canvas->getDeviceClipBounds()))) return;
    // The intermediate includes the vertical kernel's reach so the final region has no
    // repeated-edge seam. Its size remains bounded by the actual compositor target.
    workRect.outset(std::ceil(sigma * 3.f), std::ceil(sigma * 3.f));
    if (!workRect.intersect(SkRect::MakeIWH(input->width(), input->height()))) return;
    const SkIRect work = workRect.roundOut();
    workRect = SkRect::Make(work);
    const auto info = input->imageInfo().makeWH(work.width(), work.height());
    auto surface = context->createRenderTarget(info);
    if (!surface) return;

    const SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    SkRuntimeShaderBuilder builder(mProgressiveEffect);
    builder.uniform("origin") = SkV2{workRect.left(), workRect.top()};
    const SkV3 gradient{inverse.getSkewY() / bounds.height(),
                        inverse.getScaleY() / bounds.height(),
                        (inverse.getTranslateY() - bounds.top()) / bounds.height()};
    const float weakSigma = std::min(sigma * 0.13f, 8.f);
    builder.uniform("gradient") = gradient;
    builder.uniform("maxSigma") = weakSigma;
    builder.uniform("sampleStep") = SkV2{1.f, 0.f};
    auto sourceMatrix = SkMatrix::Translate(-workRect.left(), -workRect.top());
    builder.child("child") = input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                               sampling, sourceMatrix);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setShader(builder.makeShader());
    surface->getCanvas()->drawPaint(paint);

    auto horizontal = surface->makeTemporaryImage();
    builder.child("child") = horizontal->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, sampling);
    builder.uniform("sampleStep") = SkV2{0.f, 1.f};

    SkRuntimeShaderBuilder composite(mProgressiveCompositeEffect);
    composite.uniform("origin") = SkV2{workRect.left(), workRect.top()};
    composite.uniform("gradient") = gradient;
    composite.child("sharp") = builder.makeShader();
    const float sigmas[] = {weakSigma, sigma * 0.4f, sigma};
    const char* names[] = {"weak", "medium", "strong"};
    for (int i = 0; i < 3; i++) {
        auto level = context->createRenderTarget(info);
        if (!level) return;
        SkPaint gaussian;
        gaussian.setBlendMode(SkBlendMode::kSrc);
        gaussian.setImageFilter(SkImageFilters::Blur(sigmas[i], sigmas[i], SkTileMode::kClamp, nullptr));
        level->getCanvas()->drawImage(input, -workRect.left(), -workRect.top(), sampling, &gaussian);
        auto image = level->makeImageSnapshot();
        composite.child(names[i]) = image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, sampling);
    }
    auto matrix = getShaderTransform(canvas, workRect, 1.f, 1.f);
    paint.setShader(composite.makeShader(&matrix));
    paint.setAlphaf(alpha);
    paint.setBlendMode(alpha == 1.f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    paint.setAntiAlias(!effectRegion.isRect());
    canvas->drawRRect(effectRegion, paint);
}

void BlurFilter::drawBlurRegion(SkCanvas* canvas, const SkRRect& effectRegion,
                                const uint32_t blurRadius, const float zoomScale,
                                const float blurAlpha,
                                const SkRect& blurRect, sk_sp<SkImage> blurredImage,
                                sk_sp<SkImage> input) {
    SFTRACE_CALL();

    SkPaint paint;
    paint.setAlphaf(blurAlpha);

    auto blurMatrix = getShaderTransform(canvas, blurRect, kInverseInputScale, zoomScale);

    SkSamplingOptions linearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    const auto blurShader = blurredImage->makeShader(SkTileMode::kMirror, SkTileMode::kMirror,
                                                     linearSampling, &blurMatrix);

    if (blurRadius < mMaxCrossFadeRadius) {
        LOG_ALWAYS_FATAL_IF(!input);

        // For sampling Skia's API expects the inverse of what logically seems appropriate. In this
        // case you might expect the matrix to simply be the canvas matrix.
        SkMatrix inputMatrix;
        if (!canvas->getTotalMatrix().invert(&inputMatrix)) {
            ALOGE("matrix was unable to be inverted");
        }
        if (zoomScale != 1.0f) {
            inputMatrix.preTranslate(
                    blurRect.width() * (1 - zoomScale) / 2.0f,
                    blurRect.height() * (1 - zoomScale) / 2.0f);
            inputMatrix.preScale(zoomScale, zoomScale);
        }

        SkRuntimeShaderBuilder blurBuilder(mMixEffect);
        blurBuilder.child("blurredInput") = blurShader;
        blurBuilder.child("originalInput") =
                input->makeShader(SkTileMode::kMirror, SkTileMode::kMirror, linearSampling,
                                  inputMatrix);

        blurBuilder.uniform("mixFactor") = blurRadius / mMaxCrossFadeRadius;

        paint.setShader(blurBuilder.makeShader());
    } else {
        paint.setShader(blurShader);
    }

    if (effectRegion.isRect()) {
        if (blurAlpha == 1.0f) {
            paint.setBlendMode(SkBlendMode::kSrc);
        }
        canvas->drawRect(effectRegion.rect(), paint);
    } else {
        paint.setAntiAlias(true);
        canvas->drawRRect(effectRegion, paint);
    }
}

} // namespace skia
} // namespace renderengine
} // namespace android

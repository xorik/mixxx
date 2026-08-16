// The "Spectrum" waveform: the colour model measured from screenshots of
// Traktor Pro (see the MIX-5 research). Structurally this is
// res/shaders/rgbsignal.frag with three differences:
//   * the color of a column is read from a smoothed grid while the amplitude
//     keeps the full detail (in Traktor the color comes from a precomputed
//     coarse analysis, roughly 60 cells per second, stretched over the screen
//     columns by interpolation);
//   * the edge of the waveform fades out analytically over a few pixels
//     instead of being a binary inside/outside test;
//   * quiet parts do not collapse to nothing, they keep a minimum height.
// The per band color gain is the measured balance between the three bands. It
// is applied to the color only, never to the height: in Traktor the height
// does not depend on the frequency.

uniform highp vec2 framebufferSize;
uniform highp vec4 axesColor;
uniform highp vec4 lowColor;
uniform highp vec4 midColor;
uniform highp vec4 highColor;
uniform bool splitStereoSignal;

uniform int waveformLength;
uniform int textureSize;
uniform int textureStride;

uniform highp float allGain;
uniform highp float lowGain;
uniform highp float midGain;
uniform highp float highGain;
uniform highp float firstVisualIndex;
uniform highp float lastVisualIndex;

// Radius (in visual bins) of the window the color is averaged over. 0.0
// disables the smoothing, which gives the color grid of the stock RGB
// waveform.
uniform highp float colorSmoothBins;
// How many sub columns are sampled inside one framebuffer pixel. Within a
// single pixel the signal has time to rise and fall, so the alpha of that pixel
// is the share of it the waveform covers, not a yes or no about its centre.
uniform highp float subColumnSamples;
// Width of the soft edge as a fraction of the half-height of the widget, so
// that it keeps its proportion whatever the deck is scaled to.
uniform highp float softEdgeFraction;
// Lower bound for that width, in framebuffer pixels (the caller multiplies the
// wanted amount of device pixels by the oversampling factor), so the fade does
// not disappear on a small deck. Both at 0.0 give the hard edge of the stock
// RGB waveform.
uniform highp float softEdgePixels;
// Minimum visible half-height for bins that carry any signal at all, in
// [0, 1] (0.19 reproduces the plateau of 0.214 measured in Traktor).
// 0.0 disables the floor.
uniform highp float amplitudeFloor;
// Measured balance between the three bands, applied to the color only.
uniform highp vec3 bandColorGain;
// Compression of the band values before they are turned into a color. Traktor
// stores the square root of the band magnitude, so 0.5 imitates its
// compression on top of the data Mixxx has today. 1.0 leaves the data as is.
uniform highp float colorGamma;
// How strongly the column is shaded from its centre to its edge, in [0, 1].
// 0 gives the flat fill of the other waveform types.
uniform highp float verticalStrength;
// Level below which the crest factor is not trusted and the column is drawn
// flat: the bands are stored in one byte each, so on quiet columns peak and
// RMS are a few units and their ratio is noise.
uniform highp float crestLevelFloor;
// Level below which the color is no longer normalized to full brightness.
// Without it a column that carries almost nothing (the noise of a quiet
// passage) is divided by its own maximum and comes out as a fully saturated
// color decided by noise. With it, quiet columns simply get dark, which is
// also what Traktor does. 0.0 restores the plain normalization.
uniform highp float colorLevelFloor;

uniform sampler2D waveformDataTexture;

// The color window never uses more taps than this on each side, whatever
// colorSmoothBins says. GLSL 1.20 requires a constant loop bound. Every tap
// beyond the radius is skipped, so the cost follows colorSmoothBins, not this
// number.
const int kColorMaxTaps = 20;

// Upper bound for the sub columns sampled inside one pixel; GLSL 1.20 needs a
// constant loop bound. Sampling stops at subColumnSamples, so the cost follows
// that and not this number.
const int kMaxSubColumns = 8;

highp vec4 getWaveformData(highp float index) {
    highp vec2 uv_data;
    uv_data.y = floor(index / float(textureStride));
    uv_data.x = floor(index - uv_data.y * float(textureStride));
    // Divide again to convert to normalized UV coordinates.
    return texture2D(waveformDataTexture, uv_data / float(textureStride));
}

// Low/mid/high of a single visual bin, following the stereo mode. A bin holds
// two consecutive texels (left, right). The index is clamped so that the taps
// of the color window do not wrap around at the start and the end of the
// track.
highp vec3 getBands(highp float visualIndex, highp float stereoOffset) {
    highp float maxVisualIndex = floor(float(waveformLength - 1) * 0.5);
    highp float index = clamp(visualIndex, 0.0, maxVisualIndex) * 2.0;
    if (splitStereoSignal) {
        return getWaveformData(index + stereoOffset).xyz;
    }
    return max(getWaveformData(index).xyz, getWaveformData(index + 1.0).xyz);
}

// Half height of one bin as drawn, in [0, 1] of the half height of the widget:
// the signal itself and the shadow, which is the part an EQ knob has cut away.
// The amplitude floor is included, because that is the column the user sees.
highp vec2 binDistances(highp float visualIndex, highp float stereoOffset) {
    highp float maxVisualIndex = floor(float(waveformLength - 1) * 0.5);
    highp float index = clamp(visualIndex, 0.0, maxVisualIndex) * 2.0;
    highp vec4 data;
    if (splitStereoSignal) {
        data = getWaveformData(index + stereoOffset);
    } else {
        data = max(getWaveformData(index), getWaveformData(index + 1.0));
    }
    data *= allGain;
    highp vec3 scaled = data.xyz * vec3(lowGain, midGain, highGain);
    highp float sumUnscaled = data.x + data.y + data.z;
    highp float sumScaled = scaled.x + scaled.y + scaled.z;
    highp float signalDistance = data.w;
    if (sumUnscaled > 0.0) {
        signalDistance *= sumScaled / sumUnscaled;
    }
    highp float shadowDistance = data.w;
    if (amplitudeFloor > 0.0 && data.w > 0.0) {
        signalDistance = amplitudeFloor + (1.0 - amplitudeFloor) * signalDistance;
        shadowDistance = amplitudeFloor + (1.0 - amplitudeFloor) * shadowDistance;
    }
    return vec2(signalDistance, shadowDistance);
}

// The color is averaged over a window of neighbouring bins with triangular
// weights. The window is anchored to the track (it is centered on the bin, not
// on the screen column), so the color does not crawl while scrolling and the
// smoothing has no visible grid of its own.
highp vec3 smoothedBands(highp float visualIndex, highp float stereoOffset) {
    highp vec3 acc = getBands(visualIndex, stereoOffset);
    highp float weightSum = 1.0;
    for (int i = 1; i <= kColorMaxTaps; i++) {
        highp float tap = float(i);
        if (tap > colorSmoothBins) {
            break;
        }
        highp float weight = 1.0 - tap / (colorSmoothBins + 1.0);
        acc += weight *
                (getBands(visualIndex - tap, stereoOffset) +
                        getBands(visualIndex + tap, stereoOffset));
        weightSum += 2.0 * weight;
    }
    return acc / weightSum;
}

// Complementary error function, Abramowitz and Stegun 7.1.26, for x >= 0.
// Error below 1.5e-7, far under anything visible in eight bit alpha.
highp float erfc(highp float x) {
    highp float t = 1.0 / (1.0 + 0.3275911 * x);
    highp float poly = t *
            (0.254829592 +
                    t *
                            (-0.284496736 +
                                    t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    return poly * exp(-x * x);
}

// Vertical shading of a column: the alpha at distance `inside` from its centre,
// where 0 is the centre line and 1 the tip of the envelope.
//
// This is the distribution of the amplitude inside the column - the share of
// the samples in this bin whose magnitude reaches a given level - which is what
// the alpha channel of Traktor was measured to follow. A steady tone spends
// most of its time near its extremes, so the share stays high all the way out;
// impulsive material sits far below its peak almost always, so it falls away
// quickly. Which of the two a column is comes from its crest factor, and we
// have that: `all` is the peak of the bin and the three bands are RMS values.
//
//   tone         (2/pi) * acos(t)      the arcsine distribution
//   noise-like   erfc(t * crest / √2)  a Gaussian with that crest factor
//   between      linear in the crest factor from 1.41 (a sine) to 3.0
//
// Two properties come out of the model rather than being arranged: the centre
// is always fully opaque, so the axis line underneath can never show through,
// and the shading spreads over the whole height instead of sitting at one end.
// An earlier version used a band of thinning inside the body, a shape that
// exists nowhere in a signal, and it read as uniform haze.
//
// It only works because the analyzer scales all four stored values - the peak
// and the three bands - with the SAME factor. If bands are ever normalized on
// their own, the ratio silently stops meaning anything. There is a warning
// about this in analyzerwaveform.h as well.
highp float verticalProfile(highp float inside, highp float peak, highp vec3 bands) {
    if (verticalStrength <= 0.0) {
        return 1.0;
    }
    highp float rms = length(bands);
    if (rms < crestLevelFloor || peak <= 0.0) {
        return 1.0;
    }
    highp float crest = peak / rms;
    highp float t = clamp(inside, 0.0, 1.0);
    highp float tone = 0.636619772 * acos(t);
    highp float noiseLike = erfc(t * crest * 0.707106781);
    highp float weight = clamp((crest - 1.41) / (3.0 - 1.41), 0.0, 1.0);
    return mix(1.0, mix(tone, noiseLike, weight), verticalStrength);
}

// Linearly combine the low, mid, and high colors according to the low, mid,
// and high components, then normalize to the brightest component.
highp vec3 bandColor(highp vec3 data) {
    if (colorGamma != 1.0) {
        data = pow(max(data, vec3(0.0)), vec3(colorGamma));
    }
    data *= bandColorGain;
    highp vec3 color = lowColor.rgb * data.x + midColor.rgb * data.y + highColor.rgb * data.z;
    highp float maxComponent = max(color.r, max(color.g, color.b));
    // The level floor is expressed in band units, so it has to be compared
    // against a colour that has already been multiplied by the band gain. The
    // largest of the three gains is the scale between the two: without it the
    // floor was measured against a colour up to seven times larger than the
    // band it came from and practically never engaged.
    highp float gainScale = max(bandColorGain.x, max(bandColorGain.y, bandColorGain.z));
    highp float norm = max(maxComponent, colorLevelFloor * gainScale);
    if (norm > 0.0) {
        color /= norm;
    }
    return color;
}

void main(void) {
    highp vec2 uv = gl_TexCoord[0].st;
    highp float pixelY = gl_FragCoord.y;

    highp float indexRange = lastVisualIndex - firstVisualIndex;
    highp float visualIndex = floor(firstVisualIndex + uv.x * indexRange);
    highp float currentIndex = visualIndex * 2.0;

    // Coverage of this fragment by the signal and by the "shadow" (the part of
    // the waveform an EQ knob has cut away), both in [0, 1].
    highp float signalCoverage = 0.0;
    highp float shadowCoverage = 0.0;
    highp float bodyCoverage = 0.0;
    highp float verticalShading = 1.0;
    highp vec3 signalRgb = vec3(0.0);
    highp vec3 shadowRgb = vec3(0.0);

    if (currentIndex >= 0.0 && currentIndex <= float(waveformLength - 1)) {
        // Texture coordinates put (0,0) at the bottom left, so show the right
        // channel if we are in the bottom half.
        highp float stereoOffset = (uv.y < 0.5) ? 1.0 : 0.0;

        highp vec4 dataUnscaled;
        if (splitStereoSignal) {
            dataUnscaled = getWaveformData(currentIndex + stereoOffset);
        } else {
            highp vec4 left = getWaveformData(currentIndex);
            highp vec4 right = getWaveformData(currentIndex + 1.0);
            dataUnscaled = max(left, right);
        }

        highp vec3 gains = vec3(lowGain, midGain, highGain);

        dataUnscaled *= allGain;
        highp vec3 data = dataUnscaled.xyz * gains;

        // The color is read from the smoothed grid, the height below from the
        // single bin under this fragment. Note that neither the smoothed
        // values nor bandColorGain may leak into the height, or the height
        // would become frequency dependent.
        highp vec3 colorUnscaled = smoothedBands(visualIndex, stereoOffset) * allGain;
        highp vec3 colorScaled = colorUnscaled * gains;

        // ourDistance represents the [0, 1] distance of this pixel from the
        // center line.
        highp float ourDistance = abs(uv.y - 0.5) * 2.0;

        // Coverage of this pixel by the column, supersampled across it.
        //
        // A screen pixel spans several bins - at the usual zoom about five of
        // them - and the signal rises and falls inside it. What the pixel shows
        // is the share of its area the waveform covers, so the sub columns are
        // sampled separately and averaged, each with its own analytic coverage
        // in the vertical direction. That is a closed form of supersampling the
        // mask: against a reference that supersamples 8 by 8 the largest
        // difference is 0.021 of alpha and the average is 0.002.
        //
        // The count is per FRAMEBUFFER pixel, and the frame buffer is already
        // oversampled four times relative to the screen, so two sub columns
        // here are eight per screen pixel.
        //
        // Note what happens when the waveform is zoomed far in: fewer than one
        // bin falls inside a pixel, the sub columns land on the same bin and
        // the averaging stops doing anything. That is correct rather than
        // broken - at that zoom there is no detail inside a pixel to average -
        // but the effect does fade out, and the reason is worth knowing before
        // hunting for a bug. The bench this was measured on had about 3000
        // peaks per second of audio to work with; Mixxx stores 441.
        highp float indicesPerPixel = indexRange / max(framebufferSize.x, 1.0);
        highp float centreIndex = firstVisualIndex + uv.x * indexRange;
        highp float softness = max(softEdgeFraction,
                max(softEdgePixels * 2.0 / framebufferSize.y, 1e-6));
        highp float samples = max(subColumnSamples, 1.0);
        highp float signalSum = 0.0;
        highp float shadowSum = 0.0;
        for (int k = 0; k < kMaxSubColumns; k++) {
            highp float step = float(k);
            if (step >= samples) {
                break;
            }
            highp float offset = (step + 0.5) / samples - 0.5;
            highp vec2 distances =
                    binDistances(floor(centreIndex + offset * indicesPerPixel), stereoOffset);
            signalSum += clamp((distances.x - ourDistance) / softness + 0.5, 0.0, 1.0);
            shadowSum += clamp((distances.y - ourDistance) / softness + 0.5, 0.0, 1.0);
        }
        signalCoverage = signalSum / samples;
        shadowCoverage = shadowSum / samples;

        // How much of this fragment the waveform covers geometrically, before
        // the shading makes parts of it translucent. The axis line below hides
        // behind that, not behind the shaded alpha, so that making the body
        // more transparent never brings the axis back out through it.
        bodyCoverage = max(signalCoverage, shadowCoverage);

        // The shading belongs to the column, not to the layers it is built
        // from, so it is applied to the finished composite below rather than to
        // the signal and the shadow one by one. Shading them separately lets
        // the shadow, which sits under the signal at 40%, show through wherever
        // the signal was made translucent: that added up to a tenth of alpha in
        // the middle of a column and pulled the profile away from the
        // distribution it is meant to follow.
        //
        // The profile is measured against the column as drawn, including the
        // amplitude floor: the floor exists to make quiet parts visible, and a
        // visible column that is hollow inside would defeat it.
        highp vec2 centreDistances = binDistances(visualIndex, stereoOffset);
        verticalShading = verticalProfile(ourDistance / max(centreDistances.x, 1e-4),
                dataUnscaled.w,
                dataUnscaled.xyz);

        signalRgb = bandColor(colorScaled);
        shadowRgb = bandColor(colorUnscaled);
    }

    // The signal is composited over the shadow, the shadow over the axes. The
    // colour is mixed with the coverage the two layers have on their own and
    // only the finished alpha is shaded: dividing the colour by an alpha that
    // already carries the shading scales it up and clips it, which showed as
    // hues drifting by up to fourteen degrees in the middle of a column.
    // The shadow is only what the EQ removed, so it is the difference between
    // the two coverages rather than whatever the signal leaves uncovered. With
    // the knobs at neutral the two are equal and the shadow contributes
    // nothing; the old form let it fill in the antialiased edge of every
    // column, which lifted the alpha there by up to a tenth.
    highp float shadowAlpha = 0.4 * max(shadowCoverage - signalCoverage, 0.0);
    highp float bodyAlpha = signalCoverage + shadowAlpha;
    highp vec3 waveformRgb = vec3(0.0);
    if (bodyAlpha > 0.0) {
        waveformRgb = (signalRgb * signalCoverage + shadowRgb * shadowAlpha) / bodyAlpha;
    }
    highp float waveformAlpha = bodyAlpha * verticalShading;

    highp vec4 base = vec4(0.0, 0.0, 0.0, 0.0);
    if (bodyCoverage < 1.0 && abs(framebufferSize.y / 2.0 - pixelY) <= 4.0) {
        // Draw the axes color as the lowest item on the screen.
        // TODO(owilliams): The "4" in this line makes sure the axis gets
        // rendered even when the waveform is fairly short.  Really this
        // value should be based on the size of the widget.
        base = axesColor;
        base.a *= 1.0 - bodyCoverage;
    }

    highp float outAlpha = waveformAlpha + base.a * (1.0 - waveformAlpha);
    highp vec3 outRgb = vec3(0.0);
    if (outAlpha > 0.0) {
        outRgb = (waveformRgb * waveformAlpha + base.rgb * base.a * (1.0 - waveformAlpha)) / outAlpha;
    }

    gl_FragColor = vec4(outRgb, outAlpha);
}

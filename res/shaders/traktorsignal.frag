// Waveform in the style of Traktor Pro, measured from screenshots (see the
// MIX-5 research). Structurally this is res/shaders/rgbsignal.frag with three
// differences:
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
// Width of the soft edge, in framebuffer pixels (the caller multiplies the
// wanted amount of device pixels by the oversampling factor). 0.0 gives the
// hard edge of the stock RGB waveform.
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
// Level below which the color is no longer normalized to full brightness.
// Without it a column that carries almost nothing (the noise of a quiet
// passage) is divided by its own maximum and comes out as a fully saturated
// color decided by noise. With it, quiet columns simply get dark, which is
// also what Traktor does. 0.0 restores the plain normalization.
uniform highp float colorLevelFloor;

uniform sampler2D waveformDataTexture;

// The color window never uses more taps than this on each side, whatever
// colorSmoothBins says. GLSL 1.20 requires a constant loop bound.
const int kColorMaxTaps = 12;

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

// Linearly combine the low, mid, and high colors according to the low, mid,
// and high components, then normalize to the brightest component.
highp vec3 bandColor(highp vec3 data) {
    if (colorGamma != 1.0) {
        data = pow(max(data, vec3(0.0)), vec3(colorGamma));
    }
    data *= bandColorGain;
    highp vec3 color = lowColor.rgb * data.x + midColor.rgb * data.y + highColor.rgb * data.z;
    highp float maxComponent = max(color.r, max(color.g, color.b));
    highp float norm = max(maxComponent, colorLevelFloor);
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

        highp float sumUnscaled = dataUnscaled.x + dataUnscaled.y + dataUnscaled.z;
        highp float sumScaled = data.x + data.y + data.z;

        highp float signalDistance = dataUnscaled.w;
        if (sumUnscaled > 0.0) {
            signalDistance *= sumScaled / sumUnscaled;
        }
        highp float shadowDistance = dataUnscaled.w;

        // Quiet parts do not collapse to nothing: everything that carries any
        // signal at all keeps a minimum visible height. Digital silence (an
        // exactly zero bin) stays empty.
        if (amplitudeFloor > 0.0 && dataUnscaled.w > 0.0) {
            signalDistance = amplitudeFloor + (1.0 - amplitudeFloor) * signalDistance;
            shadowDistance = amplitudeFloor + (1.0 - amplitudeFloor) * shadowDistance;
        }

        // Analytic soft edge instead of a binary inside/outside test. Besides
        // the fade this gives the waveform a subpixel height, so small ripples
        // stop snapping to whole pixels.
        highp float softness = max(softEdgePixels * 2.0 / framebufferSize.y, 1e-6);
        signalCoverage = clamp((signalDistance - ourDistance) / softness + 0.5, 0.0, 1.0);
        shadowCoverage = clamp((shadowDistance - ourDistance) / softness + 0.5, 0.0, 1.0);

        signalRgb = bandColor(colorScaled);
        shadowRgb = bandColor(colorUnscaled);
    }

    // The signal is composited over the shadow, the shadow over the axes.
    highp float shadowAlpha = 0.4 * shadowCoverage * (1.0 - signalCoverage);
    highp float waveformAlpha = signalCoverage + shadowAlpha;
    highp vec3 waveformRgb = vec3(0.0);
    if (waveformAlpha > 0.0) {
        waveformRgb = (signalRgb * signalCoverage + shadowRgb * shadowAlpha) / waveformAlpha;
    }

    highp vec4 base = vec4(0.0, 0.0, 0.0, 0.0);
    if (abs(framebufferSize.y / 2.0 - pixelY) <= 4.0) {
        // Draw the axes color as the lowest item on the screen.
        // TODO(owilliams): The "4" in this line makes sure the axis gets
        // rendered even when the waveform is fairly short.  Really this
        // value should be based on the size of the widget.
        base = axesColor;
    }

    highp float outAlpha = waveformAlpha + base.a * (1.0 - waveformAlpha);
    highp vec3 outRgb = vec3(0.0);
    if (outAlpha > 0.0) {
        outRgb = (waveformRgb * waveformAlpha + base.rgb * base.a * (1.0 - waveformAlpha)) / outAlpha;
    }

    gl_FragColor = vec4(outRgb, outAlpha);
}

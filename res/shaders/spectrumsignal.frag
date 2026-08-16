// The "Spectrum" waveform: the colour model measured from screenshots of
// Traktor Pro (see the MIX-5 research). Structurally this is
// res/shaders/rgbsignal.frag with three differences:
//   * the color of a column is read from a smoothed grid while the amplitude
//     keeps the full detail (in Traktor the color comes from a precomputed
//     coarse analysis, roughly 60 cells per second, stretched over the screen
//     columns by interpolation);
//   * the edge of the waveform is antialiased from the data: a pixel spans
//     several bins, so its coverage is worked out from the sub columns inside
//     it rather than from a single yes or no;
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

// How many sub columns are sampled inside one SCREEN pixel, and how many
// framebuffer pixels one screen pixel is made of. Within a single screen pixel
// the signal rises and falls, so its alpha is the share of it the waveform
// covers rather than a yes or no about its centre.
uniform highp float subColumnSamples;
uniform highp float pixelsPerScreenPixel;
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
// Level below which the color is no longer normalized to full brightness.
// Without it a column that carries almost nothing (the noise of a quiet
// passage) is divided by its own maximum and comes out as a fully saturated
// color decided by noise. With it, quiet columns simply get dark, which is
// also what Traktor does. 0.0 restores the plain normalization.
uniform highp float colorLevelFloor;
// Brightness of a column at its rim, as a fraction of its brightness at the
// centre line. See verticalProfile().
uniform highp float rimBrightness;

uniform sampler2D waveformDataTexture;

// The sub columns never use more taps than this, whatever subColumnSamples
// says: GLSL 1.20 needs a constant loop bound. Sampling stops at
// subColumnSamples, so the cost follows that and not this number.
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

// Band values at a fractional position between analysis bins, interpolated.
//
// A drawn column usually falls between two analysis bins, and something has to
// be done about that. Interpolating the BAND VALUES is what this does;
// interpolating the finished colour instead invents hues that are in neither
// bin, which is the same defect as averaging colour over a neighbourhood and
// is what used to put orange between a red bin and a green one.
//
// Measured against Traktor over twenty seconds, with the same model and only
// the fold from bins to pixels differing: interpolating gives 21.0 degrees of
// hue error, taking the maximum 23.8, holding the nearer bin 28.0.
//
// It is not mimicry. Traktor appears to HOLD its colour across an analysis
// block - on a deep zoom capture 0.876 of neighbouring logical columns are
// unchanged, against 0.917 predicted by holding and about 0 by interpolating.
// Interpolation wins because it absorbs the misalignment between its analysis
// grid and ours, and because it removes blockiness at high zoom, not because
// Traktor does it.
highp vec3 interpolatedBands(highp float visualIndex, highp float stereoOffset) {
    highp float base = floor(visualIndex);
    highp float fraction = visualIndex - base;
    return mix(getBands(base, stereoOffset), getBands(base + 1.0, stereoOffset), fraction);
}

// Brightness across the height of a column: full at the centre line, falling
// towards the rim.
//
// Measured on Traktor, over 1848 columns of a deck capture: 0.896 at the
// centre, 0.707 at half height, 0.240 at the rim, with the hue constant to
// within 5.6 degrees down the column. So it is a brightness envelope and
// nothing else - it does not touch the colour, only how bright it is.
//
// A quadratic through those three points fits them to better than 0.01, and a
// quadratic is what this is: 1 at the centre, rimBrightness at the rim.
//
// Applied to the FINISHED alpha rather than inside the coverage, so that it
// multiplies the column instead of competing with the soft edge. The two would
// otherwise darken the same pixels twice: the measured 0.240 already contains
// whatever Traktor does at its own edge, so gasing our edge again would give a
// rim darker than the reference rather than equal to it.
highp float verticalProfile(highp float inside) {
    highp float t = clamp(inside, 0.0, 1.0);
    return mix(1.0, rimBrightness, t * t);
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
    // Half height of the column at this fragment, kept for the brightness
    // envelope below.
    highp float bodyDistance = 1.0;
    // Distance of this fragment from the centre line, 0 at the centre and 1 at
    // the top of the widget. Declared here because the brightness envelope
    // below needs it after the block that fills it.
    highp float ourDistance = abs(uv.y - 0.5) * 2.0;
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

        // The colour is read at the exact position of this column, between the
        // two analysis bins around it; the height below comes from the single
        // bin under it. Neither the interpolated values nor bandColorGain may
        // leak into the height, or the height would become frequency dependent.
        highp vec3 colorUnscaled =
                interpolatedBands(firstVisualIndex + uv.x * indexRange, stereoOffset) * allGain;
        highp vec3 colorScaled = colorUnscaled * gains;

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
        // The span to average over is a SCREEN pixel, not a framebuffer pixel.
        // The frame buffer is several times denser than the screen, so a
        // framebuffer pixel can hold less than one bin - at the zoom the user
        // works at it holds about three quarters of one - and sub columns
        // inside it all land on the same bin, which averages nothing. The
        // filtering that turns the buffer into the picture then keeps only some
        // of those pixels, so what little was there is thrown away too.
        //
        // Averaging over the screen pixel instead makes every framebuffer pixel
        // inside it carry the same, correct coverage, and the filtering has
        // nothing left to lose.
        highp float scale = max(pixelsPerScreenPixel, 1.0);
        highp float indicesPerPixel = indexRange / max(framebufferSize.x, 1.0) * scale;
        // Snap to the middle of the screen pixel this fragment belongs to.
        // Centring the window on the fragment instead would make every
        // framebuffer pixel average a different, overlapping window - a running
        // average that smears detail across neighbouring screen pixels rather
        // than an average of each one.
        highp float screenPixel = floor(uv.x * framebufferSize.x / scale);
        highp float centreIndex =
                firstVisualIndex + (screenPixel + 0.5) * indicesPerPixel;
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
        // Half height of the column here, for the brightness envelope below.
        bodyDistance = max(binDistances(floor(centreIndex), stereoOffset).x, 1e-4);

        // How much of this fragment the waveform covers geometrically, before
        // the shading makes parts of it translucent. The axis line below hides
        // behind that, not behind the shaded alpha, so that making the body
        // more transparent never brings the axis back out through it.
        bodyCoverage = max(signalCoverage, shadowCoverage);

        signalRgb = bandColor(colorScaled);
        shadowRgb = bandColor(colorUnscaled);
    }

    // The signal is composited over the shadow, the shadow over the axes.
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
    // The brightness envelope of the column, by geometric position inside it.
    // signalDistance is where the column ends, ourDistance where this fragment
    // is, so their ratio is the position: 0 on the centre line, 1 at the rim.
    highp float waveformAlpha = bodyAlpha *
            verticalProfile(ourDistance / max(bodyDistance, 1e-4));

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

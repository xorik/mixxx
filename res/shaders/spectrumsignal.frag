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
// The brightness envelope of a column, see verticalProfile().
uniform highp float profileBody;
// Bypasses the envelope so that a measurement of geometry is not multiplied by
// a measurement of brightness. Used by the tests only.
uniform bool profileFlat;
uniform highp float profileRim;
uniform highp float profileKnee;
uniform highp float profileShape;

uniform sampler2D waveformDataTexture;

// The sub columns never use more taps than this, whatever subColumnSamples
// says: GLSL 1.20 needs a constant loop bound. Sampling stops at
// subColumnSamples, so the cost follows that and not this number.
const int kMaxSubColumns = 8;

// How many bins one sub column may cover. A sub column is a pixel wide divided
// by the number of sub columns, so at the density of a deck it covers less than
// one bin and at the widest zoom a few; four is comfortably above what the
// zoom range allows.
const int kMaxBinsPerSubColumn = 4;

highp vec4 getWaveformData(highp float index) {
    highp vec2 uv_data;
    uv_data.y = floor(index / float(textureStride));
    uv_data.x = floor(index - uv_data.y * float(textureStride));
    // The CENTRE of the texel, not its corner. Without the half, the
    // coordinate lands exactly on the boundary between two texels, and which
    // one comes back is then decided by floating point rounding: most of the
    // time the intended one, sometimes its neighbour. With GL_NEAREST there is
    // no blending to hide it, so a sub column simply reads the wrong bin.
    //
    // It showed as 85 columns of 265 agreeing with the reference perfectly
    // while 48 were out by a whole sub column, with heights that had rows of
    // margin - a pattern that no rounding of the geometry can produce.
    return texture2D(waveformDataTexture, (uv_data + 0.5) / float(textureStride));
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

// Brightness across the height of a column, as measured rather than as fitted.
//
// research/traktor/column_profile_aligned.json: 41 positions from the centre
// line to the rim, measured on a deck capture with every column aligned on its
// own knee before averaging. The alignment is what makes the curve usable -
// averaging columns without it smears the flat body into a slope that no single
// column has, and a renderer built on that slope was measurably worse than no
// profile at all.
//
// The table is here whole rather than approximated. A plateau-plus-power fit
// came within 0.019 rms of it, which is invisible on a graph and is still up to
// six points of 255 on a pixel; forty-one floats are cheaper than that
// argument.
//
// Normalised to the body of the curve, so this describes only its SHAPE. How
// bright the body itself is stays our decision - Traktor draws its waveform
// darker overall, and that is a presentation choice we do not copy.
//
// Applied to the finished alpha as a MULTIPLIER, so it scales whatever the
// coverage produced instead of replacing it: where the data already thin the
// column out, the two multiply and the column gets thinner still, which is the
// intended composition.
// GLSL 1.20 has no array initialisers, so the table is a chain of comparisons.
// Unlovely, and deliberately not replaced by a formula: see above.
const int kProfilePoints = 41;

highp float profileAt(int i) {
    if (i == 0) { return 0.9803; }
    if (i == 1) { return 0.9819; }
    if (i == 2) { return 0.9643; }
    if (i == 3) { return 0.9717; }
    if (i == 4) { return 0.9718; }
    if (i == 5) { return 0.9770; }
    if (i == 6) { return 0.9756; }
    if (i == 7) { return 0.9866; }
    if (i == 8) { return 0.9951; }
    if (i == 9) { return 1.0066; }
    if (i == 10) { return 1.0197; }
    if (i == 11) { return 1.0248; }
    if (i == 12) { return 1.0283; }
    if (i == 13) { return 1.0282; }
    if (i == 14) { return 1.0264; }
    if (i == 15) { return 1.0247; }
    if (i == 16) { return 1.0212; }
    if (i == 17) { return 1.0230; }
    if (i == 18) { return 1.0146; }
    if (i == 19) { return 0.9896; }
    if (i == 20) { return 0.9885; }
    if (i == 21) { return 0.7752; }
    if (i == 22) { return 0.7282; }
    if (i == 23) { return 0.6789; }
    if (i == 24) { return 0.6377; }
    if (i == 25) { return 0.6013; }
    if (i == 26) { return 0.5588; }
    if (i == 27) { return 0.5167; }
    if (i == 28) { return 0.4787; }
    if (i == 29) { return 0.4445; }
    if (i == 30) { return 0.4131; }
    if (i == 31) { return 0.3558; }
    if (i == 32) { return 0.3169; }
    if (i == 33) { return 0.2854; }
    if (i == 34) { return 0.2490; }
    if (i == 35) { return 0.2219; }
    if (i == 36) { return 0.1942; }
    if (i == 37) { return 0.1673; }
    if (i == 38) { return 0.1380; }
    if (i == 39) { return 0.1022; }
    if (i == 40) { return 0.0829; }
    return 0.0;
}

highp float verticalProfile(highp float inside) {
    if (profileFlat) {
        return 1.0;
    }
    highp float u = clamp(inside, 0.0, 1.0) * float(kProfilePoints - 1);
    // min() has no integer overload in GLSL 1.20, so the clamp happens in float.
    int lower = int(floor(u));
    int upper = int(min(float(lower) + 1.0, float(kProfilePoints - 1)));
    return mix(profileAt(lower), profileAt(upper), u - float(lower)) * profileBody;
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
    // Distance of this fragment from the centre line, 0 at the centre and 1 at
    // the top of the widget. Declared here because the brightness envelope
    // below needs it after the block that fills it.
    highp float ourDistance = abs(uv.y - 0.5) * 2.0;
    // Top of the column at this fragment, in whole rows from the centre line.
    highp float topRows = 1.0;
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
        // The transition from the body of a column to the background is
        // whatever the sub columns make it: a pixel is opaque where all eight
        // reach it, transparent where none do, and part way in between. There
        // is no softening term. There used to be one - a fixed blur of about a
        // pixel, inherited from before this waveform type existed - and it did
        // exactly what a constant multiplier at the rim does: it darkened the
        // edge by an amount that had nothing to do with the signal, by 14% here
        // against the reference, more on short columns than on tall ones.
        //
        // The rule this follows: the alpha of a pixel comes from the data and
        // from nothing else.
        // COVERAGE, written as the model states it and nothing more:
        //
        //     for i in 0..7:  h[i] = max of the levels its slice covers
        //     top            = max(floor(max h), 1)          in whole rows
        //     coverage(row)  = count(h[i] >= row) / 8
        //     alpha(row)     = coverage(row) * profile(row / top)
        //
        // Everything is in ROWS from the centre line, not in fractions of the
        // widget, because that is the unit the model is written in and mixing
        // the two is how the previous version lost two sub columns out of eight
        // in the middle of a column.
        highp float samples = max(subColumnSamples, 1.0);
        // (H - 1) / 2, not H / 2. Both are self consistent - a full scale
        // column fills the widget either way - but only this one measures the
        // distance and the height in the SAME unit: rows away from the centre
        // ROW. With H / 2 the scale is pixels from the geometric middle of the
        // widget, and the half row between the two costs a whole sub column
        // wherever a height lands near a row boundary. Measured: it lost 8.5%
        // of the points of the reference, all of them downward.
        // TWO DIFFERENT FACTORS, and this is the whole subtlety.
        //
        // ourDistance came from the uv of the fragment: |uv.y - 0.5| * 2, so a
        // row r of H sits at |2r - (H-1)| / H. Multiplying it by H/2 gives back
        // the distance in whole rows from the centre row - that conversion is
        // fixed by how uv was built and has nothing to choose in it.
        //
        // A stored height, on the other hand, is a fraction of the half height,
        // and the half height is (H-1)/2 rows: the centre row belongs to both
        // halves. A full scale column must reach the outermost row and no
        // further.
        //
        // Using one factor for both is what cost a whole sub column wherever a
        // height landed near a row boundary: 8.5% of the points of the
        // reference, all of them downward. Using the other one for both does
        // not fix it either - it moves the same half row to the other side.
        highp float rowsPerHalfHeight = (framebufferSize.y - 1.0) * 0.5;
        highp float ourRow = ourDistance * framebufferSize.y * 0.5;
        highp float signalSum = 0.0;
        highp float shadowSum = 0.0;
        highp float maxSignalRows = 0.0;
        for (int k = 0; k < kMaxSubColumns; k++) {
            highp float step = float(k);
            if (step >= samples) {
                break;
            }
            // A sub column is a SLICE of the pixel, not a point in it, and it
            // takes the largest bin its slice covers. A point sample loses
            // whichever neighbour is taller: measured against the reference the
            // heights were then out by a median of 2.7 rows and by up to 25, on
            // 246 of 265 columns. With the maximum they agree exactly.
            highp float sliceStart = centreIndex + (step / samples - 0.5) * indicesPerPixel;
            highp float sliceEnd = sliceStart + indicesPerPixel / samples;
            highp vec2 distances = binDistances(floor(sliceStart), stereoOffset);
            for (int b = 1; b < kMaxBinsPerSubColumn; b++) {
                highp float index = floor(sliceStart) + float(b);
                if (index >= sliceEnd) {
                    break;
                }
                distances = max(distances, binDistances(index, stereoOffset));
            }
            highp float signalRows = distances.x * rowsPerHalfHeight;
            highp float shadowRows = distances.y * rowsPerHalfHeight;
            maxSignalRows = max(maxSignalRows, signalRows);
            signalSum += signalRows >= ourRow ? 1.0 : 0.0;
            shadowSum += shadowRows >= ourRow ? 1.0 : 0.0;
        }
        signalCoverage = signalSum / samples;
        shadowCoverage = shadowSum / samples;
        // The top of a column is a whole number of rows, and it is the top of
        // the tallest sub column, not the height at the centre of the pixel.
        topRows = max(floor(maxSignalRows), 1.0);

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
    // The alpha of a column is its coverage and nothing else: the share of the
    // sub columns inside this pixel that reach this height. The body comes out
    // opaque because they all agree there, the rim translucent because they
    // disagree, and how wide that rim is follows the data rather than a curve.
    //
    // There was a brightness envelope here for a while, a quadratic from the
    // centre to the rim, fitted to a measurement of Traktor. It was wrong in a
    // way worth remembering: the curve was an AVERAGE over many columns of
    // different density, and applying an average of many columns to every
    // single column is not the same statement. It matched the bench, which
    // measured the same average, and looked wrong on screen, where each column
    // is seen on its own.
    // Coverage decides the shape of the column, this decides its brightness
    // inside. See verticalProfile(); bodyDistance is the half height of the
    // column here, so the ratio is the position from the centre line to the rim.
    highp float waveformAlpha =
            bodyAlpha * verticalProfile(ourDistance * framebufferSize.y * 0.5 / topRows);

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

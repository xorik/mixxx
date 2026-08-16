#pragma once

/// The numbers that define how the Spectrum waveform looks. They live here
/// rather than inside the renderer so that the shader test can check the very
/// values the renderer uses: a test with its own copy of the constants would
/// keep passing while the two drifted apart.
///
/// Every value was chosen by comparing frames against Traktor; the reasoning is
/// kept next to it, because that is the only thing that explains why a
/// different value would be worse.
namespace mixxx {
namespace spectrumwaveform {

/// Radius of the window the colour of a column is averaged over, in visual bins
/// (441 bins per second), so four bins is about 9 ms - the grid Traktor appears
/// to use. Wider windows score better on every number we could measure and look
/// worse: averaging over 27 ms mixes neighbouring columns together, invents
/// orange between red and green and washes out the saturation.
constexpr float kColorSmoothBins = 4.0f;

/// Width of the soft edge as a fraction of the half height of the widget, with
/// a floor in device pixels so it does not disappear on a small deck. Traktor
/// fades over 3-4 device pixels, but that was measured on a waveform 174 pixels
/// tall, i.e. about 4% of its half height: the proportion is what carries over,
/// not the pixels.
constexpr float kSoftEdgeFraction = 0.04f;
/// The floor is three device pixels rather than two because two made the fade
/// on a small deck NARROWER than the fixed three pixels it replaced: at a
/// height of 120 pixels four percent of the half height is 2.4. Moving from an
/// absolute value to a proportion has to be checked at both ends of the range,
/// not only at the end where the problem was noticed.
constexpr float kSoftEdgePixels = 3.0f;

/// Minimum visible half height of a column that carries any signal. In Traktor
/// quiet columns sit on a plateau of 0.214 of the half height whatever their
/// loudness; this reproduces it (measured 0.209 on our own frames).
constexpr float kAmplitudeFloor = 0.19f;

/// Compression of the band values before they become a colour. Traktor stores
/// the square root of the band magnitude, but the error against the reference
/// is flat in this parameter (0.2 to 2.8 degrees of hue over nine tracks
/// against a median error of 16), so we do not compress at all.
constexpr float kColorGamma = 1.0f;

/// Level below which the colour of a column is no longer normalized to full
/// brightness. Without it a column that carries almost nothing is divided by
/// its own maximum and comes out fully saturated with a hue decided by noise.
constexpr float kColorLevelFloor = 0.01f;

/// Balance between the three bands, applied to the colour only, never to the
/// height. The high band is raised by 17 dB as measured in Traktor and as the
/// RMS band magnitudes of the analyzer need; the mid band is held back to 0.7,
/// which brings the share of yellow-green columns from 6.1% to 1.2%.
///
/// Note the consequence, it is deliberate: with equal bands the result is blue,
/// not grey.
constexpr float kBandColorGainLow = 1.068f;
constexpr float kBandColorGainMid = 0.7f;
constexpr float kBandColorGainHigh = 7.111f;

/// How far the vertical shading is taken towards the distribution of the
/// amplitude inside a column, see verticalProfile() in the shader. One means
/// the profile is that distribution itself. The model is physical rather than
/// invented, so there is no reason to hold it back.
constexpr float kVerticalStrength = 1.0f;

/// Below this band level the crest factor is a ratio of a few units of one byte
/// each, i.e. noise, and the column is drawn flat instead.
constexpr float kCrestLevelFloor = 0.02f;

/// The knobs of the mixer do not reach this waveform: it draws the file, the
/// way Traktor does. The ReplayGain of the track does, because it is a property
/// of the file rather than a knob.
constexpr bool kEqAffectsDrawing = false;
constexpr bool kReplayGainAffectsHeight = true;

} // namespace spectrumwaveform
} // namespace mixxx

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
constexpr float kSoftEdgePixels = 2.0f;

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

/// Vertical shading of a column. The thinning is a band inside the body, at
/// these distances from the centre in units of the half height of the column,
/// and this wide. Tonal columns are thinned closer to the centre, percussive
/// ones closer to the rim.
///
/// The band reaches neither end on purpose. A monotonic profile was tried first
/// and failed twice over: at the centre it opened a hole through which the axis
/// line showed as a white stripe, at the rim it fell where the soft edge is
/// already fading and was invisible.
constexpr float kVerticalStrength = 0.6f;
constexpr float kDipCenterTonal = 0.35f;
constexpr float kDipCenterImpulsive = 0.75f;
constexpr float kDipWidth = 0.25f;

/// The crest factor (stored peak over stored band magnitudes) at which a column
/// is drawn flat, and how fast the shading follows it away from there.
///
/// Calibrated on data, not on theory. A sine has a crest factor of 1.41, which
/// looked like the natural neutral point, but the ratio we compute divides the
/// peak by bands that are shaped by the responses of the analyzer, which makes
/// it systematically larger. Measured over seven analysed tracks the median is
/// 2.11 to 2.51, the fifth percentile 1.28 to 1.56 and the ninety fifth 2.94 to
/// 3.25. To recompute it, read the analysis files rather than reason about
/// waveforms.
constexpr float kCrestNeutral = 2.3f;
constexpr float kCrestScale = 1.1f;

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

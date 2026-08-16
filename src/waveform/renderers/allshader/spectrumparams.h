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

/// How many sub columns are sampled inside one FRAMEBUFFER pixel to work out
/// how much of it the column covers. The frame buffer is already oversampled
/// four times relative to the screen, so two here is eight per screen pixel,
/// which is the density the reference was measured at.
constexpr float kSubColumnSamples = 2.0f;

/// Width over which the edge of a column fades. The coverage is supersampled
/// across the pixel, so the edge is antialiased from the data already and this
/// is only a floor under it - one device pixel, which is what antialiasing
/// needs and no more.
///
/// It used to be four percent of the half height, which on a tall deck is eight
/// pixels of deliberate blur on top of the antialiasing, and that reads as a
/// gradient rather than as an edge. The measurement it came from (Traktor fades
/// over 3-4 device pixels on a waveform 174 pixels tall) described a waveform
/// whose edge was NOT antialiased from the data; with the supersampling in
/// place the same softness is arrived at by drawing what is there.
constexpr float kSoftEdgeFraction = 0.0f;
constexpr float kSoftEdgePixels = 1.0f;

/// Minimum visible half height of a column that carries any signal, as a
/// fraction of the half height of the widget.
///
/// ZERO ON PURPOSE, AND NOT BECAUSE NOBODY MEASURED IT. Traktor does have such
/// a plateau: quiet columns there stop following the amplitude and sit at 0.214
/// of the half height whatever their loudness, and this constant used to be
/// 0.19 to reproduce that. The user was shown both, was told that without the
/// floor very quiet passages shrink to a thread, and chose without it. That is
/// a decision about the product, not a disagreement with the measurement, so
/// please do not restore the floor "to match Traktor".
constexpr float kAmplitudeFloor = 0.0f;

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

/// The knobs of the mixer do not reach this waveform: it draws the file, the
/// way Traktor does. The ReplayGain of the track does, because it is a property
/// of the file rather than a knob.
constexpr bool kEqAffectsDrawing = false;
constexpr bool kReplayGainAffectsHeight = true;

} // namespace spectrumwaveform
} // namespace mixxx

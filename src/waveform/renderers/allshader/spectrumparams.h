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

/// Colour is no longer averaged over neighbouring bins. Averaging finished RGB
/// invents hues that are in none of the bins it averages - that is where the
/// oranges came from - and it oversmooths: our neighbour hue jump was 1.5 to
/// 3.8 degrees where Traktor sits at 17.1. What replaces it is interpolating
/// the BAND VALUES between analysis bins, in the shader, before they become a
/// colour.

/// How many sub columns are sampled inside one SCREEN pixel to work out how
/// much of it the column covers. Eight is the density the approved reference
/// was measured at.
///
/// Per screen pixel, not per framebuffer pixel, and the difference is not
/// academic: the buffer is four times denser than the screen, at the zoom the
/// user works at a framebuffer pixel holds about three quarters of a bin, and
/// sub columns spread inside it all land on the same bin. That is how the
/// antialiasing came to be measured as working in the test bench, which renders
/// at the size of the picture, and to be absent on screen.
constexpr float kSubColumnSamples = 8.0f;

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

/// Brightness of a column at its rim, as a fraction of the centre line.
///
/// Measured on a deck capture of Traktor over 1848 columns: 0.896 at the
/// centre, 0.707 at half height, 0.240 at the rim, hue constant to within 5.6
/// degrees down the column - a brightness envelope, not a colour effect. The
/// shader fits a quadratic through those three points.
///
/// This is what a waveform needs to read as a shape rather than as a bar: a
/// snare drawn flat to its own height looks like a block, and the same snare
/// with its edges falling away looks like the spike it is. The user described
/// exactly that difference before this was measured.
constexpr float kRimBrightness = 0.240f;

/// Level below which the colour of a column is no longer normalized to full
/// brightness. Without it a column that carries almost nothing is divided by
/// its own largest channel and comes out fully saturated with a hue decided by
/// noise - a silent intro became a solid bright green stripe.
///
/// Rescaled when the band balance moved into the analyser. The number is in
/// band units, and the shader used to multiply it by the largest renderer gain,
/// which was 7.111, so the effective floor was 0.071; with the renderer gain
/// now 1.0 the same 0.01 would be seven times lower and would stop working.
/// 0.05 keeps it at the same fraction of what a loud band reaches on the new
/// scale, which is the quantity that matters.
constexpr float kColorLevelFloor = 0.05f;

/// Balance between the three bands. ONE, DELIBERATELY: the balance is the one
/// measured from Traktor and it now lives in the analyser, folded into the band
/// filters themselves, where it belongs - it is a property of how a band is
/// measured, not of how it is drawn.
///
/// Carrying it in both places is how this goes wrong silently, and the numbers
/// would multiply rather than replace one another. If a band ever looks
/// mis-weighted, the place to look is enginefilterwaveform.h.
constexpr float kBandColorGainLow = 1.0f;
constexpr float kBandColorGainMid = 1.0f;
constexpr float kBandColorGainHigh = 1.0f;

/// The knobs of the mixer do not reach this waveform: it draws the file, the
/// way Traktor does. The ReplayGain of the track does, because it is a property
/// of the file rather than a knob.
constexpr bool kEqAffectsDrawing = false;
constexpr bool kReplayGainAffectsHeight = true;

} // namespace spectrumwaveform
} // namespace mixxx

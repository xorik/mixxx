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

/// There is no softening of the edge of a column, and that is deliberate: the
/// transparency of a pixel is the share of the sub columns that reach it, and
/// nothing is added on top. A fixed blur used to live here, inherited from
/// before this waveform type; measured against the approved reference it made
/// the rim 14% too dark, and by an amount that depended on how tall the column
/// was rather than on the signal.

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


/// Brightness envelope of a column: a flat body, then a fall to the rim.
///
/// Measured on a deck capture of Traktor over 41 positions, with each column
/// aligned on its own knee before averaging - without that alignment the flat
/// body is smeared away and what is left is a slope that no single column has.
/// A plateau plus a power law fits those points with rms 0.019, no point worse
/// than 0.062.
constexpr float kProfileBody = 0.875f;
constexpr float kProfileRim = 0.070f;
constexpr float kProfileKnee = 0.50f;
constexpr float kProfileShape = 0.6f;

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

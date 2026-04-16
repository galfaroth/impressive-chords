# Expressive Chords for Schwung

This is a MIDI FX module for Schwung that generates expressive chords with strumming, tilt, and articulation controls.

## Installation

You can install this module directly from this repository on your Move device:

```bash
./scripts/install.sh install-module-github mestela/schwung-expressive-chords
```

## Parameters

- **Preset**: Select from 52 different chord presets (Indie Jazz, Guitar Voicings, etc.).
- **Base Note**: The MIDI note that triggers the chord.
- **Transpose**: Transpose the output notes.
- **Invert**: Shift notes up or down by octave (-12 to +12).
- **Strum**: Delay between notes in milliseconds (0-100ms).
- **Tilt**: Velocity ramp across notes (higher notes louder or softer).
- **Articulate**: Different strum orders (1: Default, 2: Outside-In, 3: High-to-Low, 4: Even/Odd).

## Source Code

The source code is available in this repository for reference.

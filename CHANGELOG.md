# Changelog

## [1.4.9] - 2026-11-15

### Added
- Linux (X11) screen recording: region capture through the Rust X11 backend
  (XComposite/XRender composition with own-window exclusions, MIT-SHM reads),
  encoded to MP4/MKV/AVI with software H.264/H.265 through the bundled FFmpeg.
- Linux recording audio: PulseAudio system (sink monitor) and microphone
  tracks through `libpulse-simple`, also works against PipeWire's pulse server.
- Linux recording effects: click rings, scroll indicators, and keyboard keycap
  overlays observed through XInput2 raw input (click/scroll/key/modifier
  tracking with AltGr and layout-correct printable labels).
- Linux printing, XDG autostart, and AppImage automatic updates.
- Linux packaging: AppImage and DEB artifacts in CI.
- Linux input method support: bundled fcitx5 Qt6 input-context plugin.
- Linux cursor metadata projection for pixel-only native capture backends.

### Fixed
- CI: install staged OCR assets in Windows component trees, fixing the
  v1.4.8 Windows release failure at install verification.
- Linux: keep the recording toolbar above the recording area on X11.
- Linux: link x264-enabled FFmpeg without leaking bundled png symbols.

### Changed
- Known limitations updated: click/keyboard recording effects are now
  available on X11; animated-image recording, webcam capture, and Wayland
  capture remain unavailable.

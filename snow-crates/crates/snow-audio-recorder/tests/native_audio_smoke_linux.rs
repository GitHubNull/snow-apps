#![cfg(target_os = "linux")]
//! Explicit hardware smoke: enumeration plus one second of capture per source.

use snow_audio_recorder::{AudioEvent, AudioSession, AudioSourceKind, AudioStreamConfig};
use std::time::{Duration, Instant};

#[test]
#[ignore = "opens the PulseAudio default endpoints for one second; run explicitly"]
fn pulse_endpoints_enumerate_stream_and_stop() {
    let session = AudioSession::new().expect("construct native audio session");
    let render = session
        .enumerate_render_devices()
        .expect("enumerate render devices");
    let capture = session
        .enumerate_capture_devices()
        .expect("enumerate capture devices");
    println!(
        "render devices={}, capture devices={}",
        render.len(),
        capture.len()
    );
    assert!(
        render.iter().any(|device| device.is_default),
        "a default render device is required on the desktop"
    );
    for (source, available) in [
        (
            AudioSourceKind::System,
            render
                .iter()
                .any(|device| device.is_default && device.is_active),
        ),
        (
            AudioSourceKind::Microphone,
            capture
                .iter()
                .any(|device| device.is_default && device.is_active),
        ),
    ] {
        if !available {
            println!("SKIP {source:?}: no active default endpoint");
            continue;
        }
        let mut config = AudioStreamConfig::default();
        config.system.enabled = source == AudioSourceKind::System;
        config.microphone.enabled = source == AudioSourceKind::Microphone;
        config.restart_policy.max_attempts = 1;
        let stream = session
            .start_streaming(config)
            .expect("start native endpoint stream");
        // PipeWire may take a couple of seconds to resume an idle sink for the monitor.
        let deadline = Instant::now() + Duration::from_secs(4);
        let mut packets = 0_u64;
        let mut frames = 0_u64;
        while Instant::now() < deadline {
            match stream.recv_timeout(Duration::from_millis(50)) {
                Ok(AudioEvent::Packet(packet)) => {
                    assert_eq!(packet.source, source);
                    assert_eq!(
                        packet.data.len(),
                        packet.frames as usize * usize::from(packet.format.channels)
                    );
                    assert!(packet.format.sample_rate > 0);
                    packets += 1;
                    frames += u64::from(packet.frames);
                }
                Ok(_) => {}
                Err(snow_audio_recorder::RecvTimeoutError::Timeout) => {}
                Err(snow_audio_recorder::RecvTimeoutError::Closed) => break,
            }
        }
        println!(
            "{source:?}: packets={packets}, frames={frames}, stats={:?}",
            stream.stats().snapshot()
        );
        assert!(packets > 0, "expected PCM packets from {source:?}");
        assert!(frames > 0, "expected frames from {source:?}");
    }
}

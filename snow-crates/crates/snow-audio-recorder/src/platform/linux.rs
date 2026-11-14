use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use libpulse_binding as pulse;
use libpulse_simple_binding::Simple as PulseSimple;

use crate::AudioControlHandle;
use crate::backend::{AudioBackend, AudioBackendKind, AudioRecorderEngine, EngineEvent};
use crate::convert::{AudioConverter, NativeAudioFormat, NativeSampleFormat};
use crate::device::{AudioDeviceInfo, DeviceFlow, DeviceSelector};
use crate::error::{AudioError, AudioResult};
use crate::packet::{AudioEvent, AudioPacket, AudioPacketMetadata, AudioSourceKind};
use crate::session::{AudioStreamConfig, SourceConfig};

const SAMPLE_RATE: u32 = 48_000;
const SYSTEM_CHANNELS: u8 = 2;
const MICROPHONE_CHANNELS: u8 = 1;

pub(crate) struct PulseAudioBackend;

fn value_or<T>(value: Option<T>, fallback: T) -> T {
    value.unwrap_or(fallback)
}

fn map_error(error: pulse::error::PAErr) -> AudioError {
    use pulse::error::Code;
    if error.0 == Code::Access as i32 {
        AudioError::AccessDenied
    } else if error.0 == Code::ConnectionRefused as i32
        || error.0 == Code::ConnectionTerminated as i32
    {
        AudioError::BackendUnavailable(format!("PulseAudio: {error}"))
    } else if error.0 == Code::NoEntity as i32 {
        AudioError::DeviceLost
    } else {
        AudioError::platform(anyhow::anyhow!(format!("PulseAudio: {error}")))
    }
}

struct DeviceQuery {
    sinks: Vec<AudioDeviceInfo>,
    sources: Vec<AudioDeviceInfo>,
}

fn run_until_ready(
    mainloop: &mut pulse::mainloop::standard::Mainloop,
    context: &pulse::context::Context,
) -> AudioResult<()> {
    loop {
        match mainloop.iterate(false) {
            pulse::mainloop::standard::IterateResult::Success(_) => {}
            pulse::mainloop::standard::IterateResult::Quit(_) => {
                return Err(AudioError::BackendUnavailable(
                    "PulseAudio mainloop quit before connect".into(),
                ));
            }
            pulse::mainloop::standard::IterateResult::Err(error) => {
                return Err(map_error(error));
            }
        }
        match context.get_state() {
            pulse::context::State::Ready => return Ok(()),
            pulse::context::State::Failed | pulse::context::State::Terminated => {
                return Err(AudioError::BackendUnavailable(
                    "PulseAudio connection failed".into(),
                ));
            }
            _ => {}
        }
    }
}

fn pump_until(
    mainloop: &mut pulse::mainloop::standard::Mainloop,
    done: &Rc<RefCell<bool>>,
) -> AudioResult<()> {
    while !*done.borrow() {
        match mainloop.iterate(true) {
            pulse::mainloop::standard::IterateResult::Success(_) => {}
            pulse::mainloop::standard::IterateResult::Quit(_) => break,
            pulse::mainloop::standard::IterateResult::Err(error) => {
                return Err(map_error(error));
            }
        }
    }
    Ok(())
}

fn query_devices() -> AudioResult<DeviceQuery> {
    let mut mainloop = pulse::mainloop::standard::Mainloop::new().ok_or_else(|| {
        AudioError::BackendUnavailable("PulseAudio mainloop unavailable".into())
    })?;
    let mut context = pulse::context::Context::new(&mainloop, "Snow Shot").ok_or_else(|| {
        AudioError::BackendUnavailable("PulseAudio context unavailable".into())
    })?;
    context
        .connect(None, pulse::context::FlagSet::NOFLAGS, None)
        .map_err(map_error)?;
    run_until_ready(&mut mainloop, &context)?;

    let default_sink = Rc::new(RefCell::new(None::<String>));
    let default_source = Rc::new(RefCell::new(None::<String>));
    let done = Rc::new(RefCell::new(false));
    {
        let done_cb = Rc::clone(&done);
        let default_sink = Rc::clone(&default_sink);
        let default_source = Rc::clone(&default_source);
        let operation = context.introspect().get_server_info(move |info| {
            *default_sink.borrow_mut() =
                info.default_sink_name.as_ref().map(|name| name.to_string());
            *default_source.borrow_mut() =
                info.default_source_name.as_ref().map(|name| name.to_string());
            *done_cb.borrow_mut() = true;
        });
        pump_until(&mut mainloop, &done)?;
        drop(operation);
    }

    let sinks_store = Rc::new(RefCell::new(Vec::new()));
    let sources_store = Rc::new(RefCell::new(Vec::new()));
    {
        let done = Rc::new(RefCell::new(false));
        let store = Rc::clone(&sinks_store);
        let default_sink = Rc::clone(&default_sink);
        let done_cb = Rc::clone(&done);
        let operation = context.introspect().get_sink_info_list(move |result| {
            match result {
                pulse::callbacks::ListResult::Item(info) => {
                    let Some(name) = info.name.as_ref() else {
                        return;
                    };
                    let id = name.to_string();
                    let is_default = default_sink
                        .borrow()
                        .as_deref()
                        .is_some_and(|default| default == id);
                    store.borrow_mut().push(AudioDeviceInfo {
                        name: value_or(
                            info.description.as_ref().map(|text| text.to_string()),
                            id.clone(),
                        ),
                        id,
                        is_default,
                        is_active: true,
                        flow: DeviceFlow::Render,
                    });
                }
                pulse::callbacks::ListResult::End => *done_cb.borrow_mut() = true,
                pulse::callbacks::ListResult::Error => {
                    *done_cb.borrow_mut() = true;
                }
            }
        });
        pump_until(&mut mainloop, &done)?;
        drop(operation);
    }
    {
        let done = Rc::new(RefCell::new(false));
        let store = Rc::clone(&sources_store);
        let default_source = Rc::clone(&default_source);
        let done_cb = Rc::clone(&done);
        let operation = context.introspect().get_source_info_list(move |result| {
            match result {
                pulse::callbacks::ListResult::Item(info) => {
                    if info.monitor_of_sink.is_some() {
                        return;
                    }
                    let Some(name) = info.name.as_ref() else {
                        return;
                    };
                    let id = name.to_string();
                    let is_default = default_source
                        .borrow()
                        .as_deref()
                        .is_some_and(|default| default == id);
                    store.borrow_mut().push(AudioDeviceInfo {
                        name: value_or(
                            info.description.as_ref().map(|text| text.to_string()),
                            id.clone(),
                        ),
                        id,
                        is_default,
                        is_active: true,
                        flow: DeviceFlow::Capture,
                    });
                }
                pulse::callbacks::ListResult::End => *done_cb.borrow_mut() = true,
                pulse::callbacks::ListResult::Error => {
                    *done_cb.borrow_mut() = true;
                }
            }
        });
        pump_until(&mut mainloop, &done)?;
        drop(operation);
    }
    let mut sinks = std::mem::take(&mut *sinks_store.borrow_mut());
    let mut sources = std::mem::take(&mut *sources_store.borrow_mut());
    // PipeWire may report a monitor as the default source; the recording UI needs
    // a real microphone default, so fall back to the first capture source.
    if !sinks.iter().any(|device| device.is_default)
        && let Some(first) = sinks.first_mut()
    {
        first.is_default = true;
    }
    if !sources.iter().any(|device| device.is_default)
        && let Some(first) = sources.first_mut()
    {
        first.is_default = true;
    }
    Ok(DeviceQuery { sinks, sources })
}

struct PulseSource {
    chunks: std::sync::mpsc::Receiver<(Vec<u8>, Instant)>,
    stop: Arc<std::sync::atomic::AtomicBool>,
    join: Option<std::thread::JoinHandle<()>>,
    channels: u8,
}

impl PulseSource {
    fn open(device: &str, channels: u8, packet_duration: Duration) -> AudioResult<Self> {
        let spec = pulse::sample::Spec {
            format: pulse::sample::Format::F32le,
            rate: SAMPLE_RATE,
            channels,
        };
        let simple = PulseSimple::new(
            None,
            "Snow Shot",
            pulse::stream::Direction::Record,
            Some(device),
            "Recording",
            &spec,
            None,
            None,
        )
        .map_err(map_error)?;
        let frames = ((u64::from(SAMPLE_RATE) * packet_duration.as_micros().max(1) as u64)
            / 1_000_000)
            .max(1) as usize;
        let chunk_bytes = frames * usize::from(channels) * 4;
        let (sender, chunks) = std::sync::mpsc::sync_channel(64);
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        // The first read may wait for PulseAudio to resume an idle sink, so reads run
        // on their own thread and poll drains whatever arrived without blocking.
        let join = std::thread::Builder::new()
            .name("snow-audio-pulse".into())
            .spawn(move || {
                loop {
                    if worker_stop.load(std::sync::atomic::Ordering::Acquire) {
                        break;
                    }
                    let mut buffer = vec![0u8; chunk_bytes];
                    match simple.read(&mut buffer) {
                        Ok(()) => {
                            let end = Instant::now();
                            if sender.send((buffer, end)).is_err() {
                                break;
                            }
                        }
                        Err(_) => break,
                    }
                }
            })
            .map_err(|error| AudioError::platform(anyhow::anyhow!("audio reader: {error}")))?;
        Ok(Self {
            chunks,
            stop,
            join: Some(join),
            channels,
        })
    }
}

impl Drop for PulseSource {
    fn drop(&mut self) {
        self.stop
            .store(true, std::sync::atomic::Ordering::Release);
        // Dropping the receiver unblocks a pending send. The reader may still be
        // waiting on the next chunk, so detaching is safer than joining here.
        if let Some(join) = self.join.take() {
            drop(join);
        }
    }
}

fn system_device(selector: &DeviceSelector) -> AudioResult<String> {
    match selector {
        DeviceSelector::DefaultRender => Ok("@DEFAULT_MONITOR@".into()),
        DeviceSelector::Id(name) => Ok(format!("{name}.monitor")),
        DeviceSelector::DefaultCapture => Err(AudioError::InvalidConfig(
            "system audio requires a render device".into(),
        )),
    }
}

fn microphone_device(selector: &DeviceSelector) -> AudioResult<String> {
    match selector {
        // A monitor can be the server default source, so resolve the actual
        // microphone through enumeration instead of trusting @DEFAULT_SOURCE@.
        DeviceSelector::DefaultCapture => Err(AudioError::InvalidConfig(
            "default capture requires device enumeration".into(),
        )),
        DeviceSelector::Id(name) => Ok(name.clone()),
        DeviceSelector::DefaultRender => Err(AudioError::InvalidConfig(
            "microphone requires a capture device".into(),
        )),
    }
}

fn default_microphone_source() -> AudioResult<String> {
    let query = query_devices()?;
    query
        .sources
        .iter()
        .find(|device| device.is_default)
        .or_else(|| query.sources.first())
        .map(|device| device.id.clone())
        .ok_or_else(|| AudioError::DeviceUnavailable("no microphone source available".into()))
}

fn start_source(
    source: AudioSourceKind,
    config: &SourceConfig,
    device: &str,
    channels: u8,
    control: Option<&AudioControlHandle>,
    states: &mut [crate::AudioSourceStatus; 2],
) -> AudioResult<Option<PulseSource>> {
    let index = crate::control::source_index(source);
    if !config.enabled {
        states[index] = crate::AudioSourceStatus::Disabled;
    } else {
        match PulseSource::open(device, channels, config.packet_duration) {
            Ok(stream) => {
                states[index] = crate::AudioSourceStatus::Ready;
                if let Some(control) = control {
                    control.set_source_status(source, states[index]);
                }
                return Ok(Some(stream));
            }
            Err(error) => {
                states[index] = if matches!(error, AudioError::AccessDenied) {
                    crate::AudioSourceStatus::PermissionDenied
                } else {
                    crate::AudioSourceStatus::Unavailable
                };
                if let Some(control) = control {
                    control.set_source_status(source, states[index]);
                }
                if config.required {
                    return Err(error);
                }
            }
        }
    }
    if let Some(control) = control {
        control.set_source_status(source, states[index]);
    }
    Ok(None)
}

impl AudioBackend for PulseAudioBackend {
    fn enumerate_devices(&self, flow: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
        let query = query_devices()?;
        Ok(match flow {
            DeviceFlow::Render => query.sinks,
            DeviceFlow::Capture => query.sources,
        })
    }

    fn create_engine(
        &self,
        config: AudioStreamConfig,
    ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
        Self::create_engine_impl(config, None)
    }

    fn create_engine_with_controls(
        &self,
        config: AudioStreamConfig,
        control: &AudioControlHandle,
    ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
        Self::create_engine_impl(config, Some(control))
    }
}

impl PulseAudioBackend {
    fn create_engine_impl(
        config: AudioStreamConfig,
        control: Option<&AudioControlHandle>,
    ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
        config.validate()?;
        let mut states = [crate::AudioSourceStatus::Disabled; 2];
        let mut sources = [None, None];
        if config.system.enabled {
            let device = system_device(&config.system.device)?;
            sources[0] = start_source(
                AudioSourceKind::System,
                &config.system,
                &device,
                SYSTEM_CHANNELS,
                control,
                &mut states,
            )?;
        } else if let Some(control) = control {
            control.set_source_status(AudioSourceKind::System, states[0]);
        }
        if config.cancellation.is_canceled() {
            return Err(AudioError::Canceled);
        }
        if config.microphone.enabled {
            let device = match &config.microphone.device {
                DeviceSelector::DefaultCapture => default_microphone_source()?,
                selector => microphone_device(selector)?,
            };
            sources[1] = start_source(
                AudioSourceKind::Microphone,
                &config.microphone,
                &device,
                MICROPHONE_CHANNELS,
                control,
                &mut states,
            )?;
        } else if let Some(control) = control {
            control.set_source_status(AudioSourceKind::Microphone, states[1]);
        }
        if sources[0].is_none() && sources[1].is_none() {
            return Err(AudioError::DeviceUnavailable(
                "no PulseAudio source started".into(),
            ));
        }
        Ok(Box::new(Engine {
            config,
            sources,
            states,
            converters: [None, None],
            formats: [None, None],
            sequence: [0, 0],
        }))
    }
}

struct Engine {
    config: AudioStreamConfig,
    sources: [Option<PulseSource>; 2],
    states: [crate::AudioSourceStatus; 2],
    converters: [Option<AudioConverter>; 2],
    formats: [Option<(u32, u16)>; 2],
    sequence: [u64; 2],
}

impl Engine {
    fn packet(
        &mut self,
        index: usize,
        channels: u8,
        bytes: &[u8],
        end: Instant,
    ) -> AudioResult<AudioEvent> {
        let output = if index == 0 {
            self.config.system.output_format
        } else {
            self.config.microphone.output_format
        };
        let native = (SAMPLE_RATE, u16::from(channels));
        if self.formats[index] != Some(native) {
            self.converters[index] = Some(AudioConverter::new(
                NativeAudioFormat {
                    sample_rate: SAMPLE_RATE,
                    channels: native.1,
                    sample_format: NativeSampleFormat::F32,
                },
                output,
            )?);
            self.formats[index] = Some(native);
        }
        let source_frames = (bytes.len() / (usize::from(channels) * 4)) as u32;
        let data = self.converters[index]
            .as_mut()
            .expect("converter initialized")
            .convert_chunk(bytes, source_frames)?;
        let frames = data.len() / usize::from(output.channels);
        self.sequence[index] = self.sequence[index].wrapping_add(1);
        let metadata = AudioPacketMetadata {
            sequence: self.sequence[index],
            stream_timestamp: Some(snow_core::timestamp::StreamTimestamp {
                instant: end,
                raw_os_ticks: None,
                tick_format: snow_core::timestamp::TickFormat::RawQpc,
            }),
            ..Default::default()
        };
        Ok(AudioEvent::Packet(AudioPacket {
            source: if index == 0 {
                AudioSourceKind::System
            } else {
                AudioSourceKind::Microphone
            },
            format: output,
            frames: frames as u32,
            data,
            metadata,
        }))
    }
}

impl AudioRecorderEngine for Engine {
    fn source_states(&self) -> Option<[crate::AudioSourceStatus; 2]> {
        Some(self.states)
    }

    fn poll(&mut self, timeout: Duration) -> AudioResult<EngineEvent> {
        if self.config.cancellation.is_canceled() {
            return Err(AudioError::Canceled);
        }
        let mut events = Vec::with_capacity(8);
        for index in 0..2 {
            let Some(source) = self.sources[index].take() else {
                continue;
            };
            while let Ok((buffer, end)) = source.chunks.try_recv() {
                events.push(self.packet(index, source.channels, &buffer, end)?);
                if self.config.cancellation.is_canceled() {
                    self.sources[index] = Some(source);
                    return Err(AudioError::Canceled);
                }
            }
            self.sources[index] = Some(source);
        }
        if events.is_empty() {
            // The reader threads deliver on their own cadence; park instead of spinning.
            let slice = Duration::from_millis(10);
            let mut remaining = timeout.min(Duration::from_millis(100));
            while !remaining.is_zero() {
                if self.config.cancellation.is_canceled() {
                    return Err(AudioError::Canceled);
                }
                let step = remaining.min(slice);
                std::thread::sleep(step);
                remaining = remaining.saturating_sub(step);
                for index in 0..2 {
                    let Some(source) = self.sources[index].take() else {
                        continue;
                    };
                    while let Ok((buffer, end)) = source.chunks.try_recv() {
                        events.push(self.packet(index, source.channels, &buffer, end)?);
                    }
                    self.sources[index] = Some(source);
                }
                if !events.is_empty() {
                    break;
                }
            }
        }
        if events.is_empty() {
            Ok(EngineEvent::Idle)
        } else {
            Ok(EngineEvent::Events(events))
        }
    }
}

pub(crate) fn build_pulse_backend(kind: AudioBackendKind) -> AudioResult<Arc<dyn AudioBackend>> {
    if !matches!(kind, AudioBackendKind::Auto) {
        return Err(AudioError::BackendUnavailable(
            "the selected audio backend is not available on Linux".into(),
        ));
    }
    Ok(Arc::new(PulseAudioBackend))
}

#[cfg(test)]
mod rate_probe {
    use super::*;

    #[test]
    #[ignore = "measures monitor delivery rate for five seconds; run explicitly"]
    fn pulse_monitor_delivery_rate() {
        let source =
            PulseSource::open("@DEFAULT_MONITOR@", 2, Duration::from_millis(10)).unwrap();
        let wall = Instant::now();
        let Ok((buffer, _)) = source.chunks.recv_timeout(Duration::from_secs(5)) else {
            eprintln!("no first chunk within five seconds");
            return;
        };
        let mut frames = (buffer.len() / 8) as u64;
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(5) {
            let Ok((buffer, _)) = source.chunks.recv_timeout(Duration::from_millis(500)) else {
                break;
            };
            frames += (buffer.len() / 8) as u64;
        }
        eprintln!(
            "first_chunk_after={:?} active={:?} frames={frames} audio_seconds={:.2}",
            wall.elapsed(),
            start.elapsed(),
            frames as f64 / 48000.0
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use pulse::error::{Code, PAErr};

    #[test]
    fn system_device_maps_selectors_to_monitor_sources() {
        assert_eq!(
            system_device(&DeviceSelector::DefaultRender).unwrap(),
            "@DEFAULT_MONITOR@"
        );
        assert_eq!(
            system_device(&DeviceSelector::Id("alsa_output.usb".into())).unwrap(),
            "alsa_output.usb.monitor"
        );
        assert!(matches!(
            system_device(&DeviceSelector::DefaultCapture),
            Err(AudioError::InvalidConfig(_))
        ));
    }

    #[test]
    fn microphone_device_maps_selectors_to_capture_sources() {
        assert_eq!(
            microphone_device(&DeviceSelector::Id("alsa_input.usb".into())).unwrap(),
            "alsa_input.usb"
        );
        assert!(matches!(
            microphone_device(&DeviceSelector::DefaultCapture),
            Err(AudioError::InvalidConfig(_))
        ));
        assert!(matches!(
            microphone_device(&DeviceSelector::DefaultRender),
            Err(AudioError::InvalidConfig(_))
        ));
    }

    #[test]
    fn pulse_errors_map_to_audio_error_classes() {
        assert!(matches!(
            map_error(PAErr(Code::Access as i32)),
            AudioError::AccessDenied
        ));
        assert!(matches!(
            map_error(PAErr(Code::ConnectionRefused as i32)),
            AudioError::BackendUnavailable(_)
        ));
        assert!(matches!(
            map_error(PAErr(Code::NoEntity as i32)),
            AudioError::DeviceLost
        ));
        assert!(matches!(
            map_error(PAErr(Code::Invalid as i32)),
            AudioError::Platform(_)
        ));
    }

    #[test]
    fn engine_packets_match_configured_output_frames() {
        let mut config = AudioStreamConfig::default();
        config.system.output_format = crate::format::AudioFormat::new(48_000, 2);
        config.microphone.enabled = false;
        let mut engine = Engine {
            config,
            sources: [None, None],
            states: [crate::AudioSourceStatus::Disabled; 2],
            converters: [None, None],
            formats: [None, None],
            sequence: [0, 0],
        };
        let bytes = vec![0u8; 480 * 2 * 4];
        let event = engine
            .packet(0, 2, &bytes, Instant::now())
            .expect("packet conversion");
        let AudioEvent::Packet(packet) = event else {
            panic!("expected packet event");
        };
        assert_eq!(packet.source, AudioSourceKind::System);
        assert_eq!(packet.frames, 480);
        assert_eq!(packet.data.len(), 480 * 2);
        assert_eq!(packet.metadata.sequence, 1);
        assert!(packet.metadata.stream_timestamp.is_some());
    }
}


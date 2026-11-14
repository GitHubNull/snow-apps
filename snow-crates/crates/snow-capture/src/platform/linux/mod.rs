//! Linux X11 capture backend.
//!
//! Monitors come from RandR, pixels from MIT-SHM (with a socket fallback), and
//! recording exclusions from an XRender composition of the redirected
//! top-level windows so Snow Shot's own toolbars never appear in the output.

mod x11;

use std::sync::{Arc, Mutex};

use x11rb::protocol::xproto;

use crate::backend::{
    CaptureBackend, CaptureBackendKind, CaptureMode, CaptureSampleMetadata, MonitorCapturer,
};
use crate::capture_session::CaptureTargetInfo;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::monitor::{MonitorDesktopGeometry, MonitorId};
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::window::WindowId;
use snow_cursor::CursorSampler;

use x11::X11CaptureConnection;

pub(crate) struct LinuxBackend {
    connection: Arc<Mutex<X11CaptureConnection>>,
}

impl LinuxBackend {
    pub(crate) fn new() -> CaptureResult<Self> {
        Ok(Self {
            connection: Arc::new(Mutex::new(X11CaptureConnection::new()?)),
        })
    }

    fn monitors_from(&self, connection: &X11CaptureConnection) -> CaptureResult<Vec<MonitorId>> {
        let monitors = connection.monitors()?;
        Ok(monitors
            .into_iter()
            .map(|monitor| {
                MonitorId::from_name(monitor.xid as isize, monitor.name, monitor.primary)
                    .with_desktop_geometry(MonitorDesktopGeometry {
                        x: f64::from(monitor.x),
                        y: f64::from(monitor.y),
                        width: f64::from(monitor.width),
                        height: f64::from(monitor.height),
                        pixel_width: monitor.width,
                        pixel_height: monitor.height,
                    })
            })
            .collect())
    }

    fn geometry_for(&self, monitor: &MonitorId) -> CaptureResult<(i32, i32, u32, u32)> {
        if let Some(geometry) = monitor.desktop_geometry() {
            return Ok((
                geometry.x as i32,
                geometry.y as i32,
                geometry.pixel_width,
                geometry.pixel_height,
            ));
        }
        let connection = self
            .connection
            .lock()
            .map_err(|_| CaptureError::platform(anyhow::anyhow!("capture connection poisoned")))?;
        for monitor_info in connection.monitors()? {
            if monitor_info.name == monitor.name() {
                return Ok((
                    monitor_info.x,
                    monitor_info.y,
                    monitor_info.width,
                    monitor_info.height,
                ));
            }
        }
        Err(CaptureError::MonitorLost)
    }

    fn capturer(
        &self,
        target: LinuxTarget,
        options: &crate::CaptureOptions,
    ) -> Box<dyn MonitorCapturer> {
        Box::new(LinuxCapturer {
            connection: Arc::clone(&self.connection),
            target,
            excluded_windows: Arc::clone(&options.excluded_windows),
            excluded_processes: Arc::clone(&options.excluded_processes),
            output_format: options.output_pixel_format,
            last_hash: None,
            cursor_sampler: None,
        })
    }
}

impl CaptureBackend for LinuxBackend {
    fn create_target_capturer(
        &self,
        target: &crate::CaptureTarget,
        options: crate::CaptureOptions,
    ) -> CaptureResult<Option<Box<dyn MonitorCapturer>>> {
        let linux_target = match target {
            crate::CaptureTarget::PrimaryMonitor => {
                let primary = self.primary_monitor()?;
                let (x, y, width, height) = self.geometry_for(&primary)?;
                LinuxTarget::Region { x, y, width, height }
            }
            crate::CaptureTarget::Monitor(monitor) => {
                let (x, y, width, height) = self.geometry_for(monitor)?;
                LinuxTarget::Region { x, y, width, height }
            }
            crate::CaptureTarget::Region(region) => LinuxTarget::Region {
                x: region.x,
                y: region.y,
                width: region.width,
                height: region.height,
            },
            crate::CaptureTarget::Window(window) => {
                LinuxTarget::Window(window.linux_xid()?)
            }
        };
        Ok(Some(self.capturer(linux_target, &options)))
    }

    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        let connection = self
            .connection
            .lock()
            .map_err(|_| CaptureError::platform(anyhow::anyhow!("capture connection poisoned")))?;
        self.monitors_from(&connection)
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        let monitors = self.enumerate_monitors()?;
        monitors
            .iter()
            .find(|monitor| monitor.is_primary())
            .or_else(|| monitors.first())
            .cloned()
            .ok_or(CaptureError::MonitorLost)
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        let monitors = self.enumerate_monitors()?;
        layout_from_monitors(monitors)
    }

    fn inspect_window(&self, window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        let connection = self
            .connection
            .lock()
            .map_err(|_| CaptureError::platform(anyhow::anyhow!("capture connection poisoned")))?;
        let info = connection.inspect_window(window.linux_xid()? as xproto::Window)?;
        Ok(CaptureTargetInfo {
            origin_x: info.x,
            origin_y: info.y,
            width: info.width,
            height: info.height,
        })
    }

    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        let (x, y, width, height) = self.geometry_for(monitor)?;
        Ok(self.capturer(
            LinuxTarget::Region { x, y, width, height },
            &crate::CaptureOptions::default(),
        ))
    }

    fn create_window_capturer(
        &self,
        window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(self.capturer(
            LinuxTarget::Window(window.linux_xid()?),
            &crate::CaptureOptions::default(),
        ))
    }
}

pub(crate) fn layout_from_monitors(monitors: Vec<MonitorId>) -> CaptureResult<MonitorLayout> {
    let mut geometries = Vec::with_capacity(monitors.len());
    for monitor in monitors {
        let Some(geometry) = monitor.desktop_geometry() else {
            return Err(CaptureError::MonitorLost);
        };
        geometries.push(MonitorGeometry {
            monitor,
            x: geometry.x as i32,
            y: geometry.y as i32,
            width: geometry.pixel_width,
            height: geometry.pixel_height,
        });
    }
    let left = geometries
        .iter()
        .map(|monitor| monitor.x)
        .min()
        .ok_or(CaptureError::MonitorLost)?;
    let top = geometries
        .iter()
        .map(|monitor| monitor.y)
        .min()
        .ok_or(CaptureError::MonitorLost)?;
    let right = geometries
        .iter()
        .map(|monitor| monitor.x + monitor.width as i32)
        .max()
        .ok_or(CaptureError::MonitorLost)?;
    let bottom = geometries
        .iter()
        .map(|monitor| monitor.y + monitor.height as i32)
        .max()
        .ok_or(CaptureError::MonitorLost)?;
    Ok(MonitorLayout {
        monitors: geometries,
        virtual_left: left,
        virtual_top: top,
        virtual_width: (right - left).max(0) as u32,
        virtual_height: (bottom - top).max(0) as u32,
    })
}

#[derive(Clone, Copy)]
enum LinuxTarget {
    Region {
        x: i32,
        y: i32,
        width: u32,
        height: u32,
    },
    Window(u64),
}

struct LinuxCapturer {
    connection: Arc<Mutex<X11CaptureConnection>>,
    target: LinuxTarget,
    excluded_windows: Arc<[u32]>,
    excluded_processes: Arc<[i32]>,
    output_format: CapturePixelFormat,
    last_hash: Option<u64>,
    cursor_sampler: Option<CursorSampler>,
}

impl LinuxCapturer {
    fn capture_bytes(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        let output_rgba = self.output_format == CapturePixelFormat::Rgba8;
        let capture_time = std::time::Instant::now();
        let (bytes, width, height) = {
            let mut connection = self.connection.lock().map_err(|_| {
                CaptureError::platform(anyhow::anyhow!("capture connection poisoned"))
            })?;
            match self.target {
                LinuxTarget::Region {
                    x,
                    y,
                    width,
                    height,
                } => {
                    let (bytes, _) = connection.capture_region_rgba(
                        x,
                        y,
                        width,
                        height,
                        &self.excluded_windows,
                        &self.excluded_processes,
                        output_rgba,
                    )?;
                    (bytes, width, height)
                }
                LinuxTarget::Window(window) => {
                    let (bytes, width, height) = connection
                        .capture_window_rgba(window as xproto::Window, output_rgba)?;
                    (bytes, width, height)
                }
            }
        };
        let hash = frame_hash(&bytes);
        let duplicate = self.last_hash == Some(hash);
        self.last_hash = Some(hash);
        let mut frame = match reuse {
            Some(mut frame)
                if frame.width() == width
                    && frame.height() == height
                    && frame.pixel_format() == self.output_format =>
            {
                frame.prepare_for_overwrite(width, height, self.output_format)?;
                frame.as_mut_bytes().copy_from_slice(&bytes);
                frame
            }
            _ => match self.output_format {
                CapturePixelFormat::Rgba8 => Frame::from_rgba8(width, height, bytes)?,
                CapturePixelFormat::Bgra8 => Frame::from_bgra8(width, height, bytes)?,
            },
        };
        frame.metadata.is_duplicate = duplicate;
        frame.metadata.set_timing_with_format(
            Some(capture_time),
            None,
            snow_core::timestamp::TickFormat::RawQpc,
        );
        Ok(frame)
    }
}

impl MonitorCapturer for LinuxCapturer {
    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.capture_bytes(reuse)
    }

    fn capture_desktop_region_into(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        destination: &mut Frame,
        _destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        let output_rgba = self.output_format == CapturePixelFormat::Rgba8;
        let capture_time = std::time::Instant::now();
        let bytes = {
            let mut connection = self.connection.lock().map_err(|_| {
                CaptureError::platform(anyhow::anyhow!("capture connection poisoned"))
            })?;
            connection
                .capture_region_rgba(
                    x,
                    y,
                    width,
                    height,
                    &self.excluded_windows,
                    &self.excluded_processes,
                    output_rgba,
                )?
                .0
        };
        destination.prepare_for_overwrite(width, height, self.output_format)?;
        if destination.as_mut_bytes().len() != bytes.len() {
            return Err(CaptureError::InvalidConfig(
                "destination frame size changed while capturing".into(),
            ));
        }
        destination.as_mut_bytes().copy_from_slice(&bytes);
        Ok(Some(CaptureSampleMetadata {
            capture_time: Some(capture_time),
            raw_os_ticks: None,
            tick_format: snow_core::timestamp::TickFormat::RawQpc,
            is_duplicate: false,
            dirty_rects: Vec::new(),
        }))
    }

    fn set_capture_mode(&mut self, _mode: CaptureMode) -> CaptureResult<()> {
        Ok(())
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.output_format = format;
        Ok(())
    }

    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::X11
    }

    fn sample_cursor(&mut self) -> CaptureResult<Option<snow_cursor::CursorSnapshot>> {
        if self.cursor_sampler.is_none() {
            self.cursor_sampler = CursorSampler::new().ok();
        }
        Ok(self
            .cursor_sampler
            .as_mut()
            .and_then(|sampler| sampler.sample().ok()))
    }
}

fn frame_hash(bytes: &[u8]) -> u64 {
    // Sample a bounded number of bytes so duplicate detection never dominates
    // the capture budget on large 4K frames.
    const PRIME: u64 = 0x100000001b3;
    let mut hash = 0xcbf29ce484222325u64;
    let step = (bytes.len() / 8192).max(1);
    for byte in bytes.iter().step_by(step) {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(PRIME);
    }
    hash
}

pub(crate) fn monitor_layout_from_monitors(
    monitors: Vec<MonitorId>,
) -> CaptureResult<MonitorLayout> {
    layout_from_monitors(monitors)
}

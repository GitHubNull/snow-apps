//! X11 primitives shared by the Linux capture backend.
//!
//! All requests use one RustConnection per backend instance; the connection is
//! protocol-level and thread-safe, so monitor, window and live captures can
//! share it. Pixel transfers prefer MIT-SHM and fall back to the socket path.

use std::ffi::c_void;

use anyhow::anyhow;
use x11rb::connection::Connection;
use x11rb::protocol::composite::ConnectionExt as _;
use x11rb::protocol::randr::ConnectionExt as _;
use x11rb::protocol::render::{self, ConnectionExt as _};
use x11rb::protocol::shm::{self, ConnectionExt as _};
use x11rb::protocol::xproto::{self, ConnectionExt as _, ImageFormat};
use x11rb::rust_connection::RustConnection;

use crate::error::{CaptureError, CaptureResult};

const NET_WM_PID: &str = "_NET_WM_PID";

#[derive(Clone, Debug)]
pub(crate) struct MonitorInfo {
    pub xid: u32,
    pub name: String,
    pub primary: bool,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Debug)]
pub(crate) struct WindowInfo {
    pub window: u32,
    pub frame: u32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub process_id: u32,
    pub mapped: bool,
}

struct ShmSegment {
    segment: shm::Seg,
    address: *mut u8,
    size: usize,
}

// The mapping is owned by this connection and is only accessed while the
// enclosing mutex is held; no other thread observes the raw pointer.
unsafe impl Send for ShmSegment {}

impl Drop for ShmSegment {
    fn drop(&mut self) {
        unsafe {
            if !self.address.is_null() {
                libc::munmap(self.address.cast::<c_void>(), self.size);
            }
        }
    }
}

pub(crate) struct X11CaptureConnection {
    connection: RustConnection,
    screen_number: usize,
    root: xproto::Window,
    root_depth: u8,
    root_visual: xproto::Visualid,
    root_width: u16,
    root_height: u16,
    shm: bool,
    composite: bool,
    render: bool,
    shm_segment: Option<ShmSegment>,
}

impl X11CaptureConnection {
    pub(crate) fn new() -> CaptureResult<Self> {
        let (connection, screen_number) = x11rb::connect(None).map_err(|error| {
            CaptureError::platform(anyhow!("X11 display connection failed: {error}"))
        })?;
        let screen = &connection.setup().roots[screen_number];
        let shm = connection
            .shm_query_version()
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .is_some();
        let composite = connection
            .composite_query_version(0, 4)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .is_some();
        let render = connection
            .render_query_version(0, 11)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .is_some();
        Ok(Self {
            root: screen.root,
            root_depth: screen.root_depth,
            root_visual: screen.root_visual,
            root_width: screen.width_in_pixels,
            root_height: screen.height_in_pixels,
            screen_number,
            shm,
            composite,
            render,
            connection,
            shm_segment: None,
        })
    }

    pub(crate) fn monitors(&self) -> CaptureResult<Vec<MonitorInfo>> {
        let monitors_reply = self
            .connection
            .randr_get_monitors(self.root, true)
            .ok()
            .and_then(|cookie| cookie.reply().ok());
        if let Some(reply) = monitors_reply {
            let mut monitors = Vec::with_capacity(reply.monitors.len());
            for monitor in reply.monitors {
                let name = self
                    .connection
                    .get_atom_name(monitor.name)
                    .ok()
                    .and_then(|cookie| cookie.reply().ok())
                    .map(|reply| String::from_utf8_lossy(&reply.name).into_owned())
                    .unwrap_or_else(|| format!("monitor-{:x}", monitor.name));
                monitors.push(MonitorInfo {
                    xid: monitor.name,
                    name,
                    primary: monitor.primary,
                    x: i32::from(monitor.x),
                    y: i32::from(monitor.y),
                    width: u32::from(monitor.width),
                    height: u32::from(monitor.height),
                });
            }
            if !monitors.is_empty() {
                return Ok(monitors);
            }
        }
        Ok(vec![MonitorInfo {
            xid: 0,
            name: "screen".into(),
            primary: true,
            x: 0,
            y: 0,
            width: u32::from(self.root_width),
            height: u32::from(self.root_height),
        }])
    }

    pub(crate) fn inspect_window(&self, window: xproto::Window) -> CaptureResult<WindowInfo> {
        let attributes = self
            .connection
            .get_window_attributes(window)
            .map_err(|error| CaptureError::platform(anyhow!("window attributes: {error}")))?
            .reply()
            .map_err(|error| {
                CaptureError::InvalidTarget(format!("window {window:#x} is gone: {error}"))
            })?;
        let geometry = self
            .connection
            .get_geometry(window)
            .map_err(|error| CaptureError::platform(anyhow!("window geometry: {error}")))?
            .reply()
            .map_err(|error| {
                CaptureError::InvalidTarget(format!("window {window:#x} is gone: {error}"))
            })?;
        let frame = self.frame_for(window).unwrap_or(window);
        let (x, y) = self.translated_origin(frame)?;
        let process_id = self.window_process_id(window).unwrap_or(0);
        Ok(WindowInfo {
            window,
            frame,
            x,
            y,
            width: u32::from(geometry.width),
            height: u32::from(geometry.height),
            process_id,
            mapped: attributes.map_state == xproto::MapState::VIEWABLE,
        })
    }

    pub(crate) fn window_check(&self, window: xproto::Window) -> CaptureResult<WindowInfo> {
        self.inspect_window(window)
    }

    pub(crate) fn top_level_windows(&self) -> CaptureResult<Vec<xproto::Window>> {
        let tree = self
            .connection
            .query_tree(self.root)
            .map_err(|error| CaptureError::platform(anyhow!("query tree: {error}")))?
            .reply()
            .map_err(|error| CaptureError::platform(anyhow!("query tree reply: {error}")))?;
        Ok(tree.children)
    }

    pub(crate) fn frame_for(&self, window: xproto::Window) -> Option<xproto::Window> {
        let mut current = window;
        for _ in 0..64 {
            let tree = self.connection.query_tree(current).ok()?.reply().ok()?;
            let parent = tree.parent;
            if parent == self.root {
                return Some(current);
            }
            if parent == 0 {
                return None;
            }
            current = parent;
        }
        None
    }

    pub(crate) fn translated_origin(&self, window: xproto::Window) -> CaptureResult<(i32, i32)> {
        let reply = self
            .connection
            .translate_coordinates(window, self.root, 0, 0)
            .map_err(|error| CaptureError::platform(anyhow!("translate coordinates: {error}")))?
            .reply()
            .map_err(|error| {
                CaptureError::platform(anyhow!("translate coordinates reply: {error}"))
            })?;
        Ok((i32::from(reply.dst_x), i32::from(reply.dst_y)))
    }

    pub(crate) fn window_process_id(&self, window: xproto::Window) -> Option<u32> {
        let atom = self
            .connection
            .intern_atom(false, NET_WM_PID.as_bytes())
            .ok()?
            .reply()
            .ok()?
            .atom;
        let reply = self
            .connection
            .get_property(false, window, atom, xproto::AtomEnum::CARDINAL, 0, 1)
            .ok()?
            .reply()
            .ok()?;
        if reply.format != 32 || reply.value.len() < 4 {
            return None;
        }
        Some(u32::from_le_bytes([
            reply.value[0],
            reply.value[1],
            reply.value[2],
            reply.value[3],
        ]))
    }

    /// Captures a root-space rectangle. Windows whose client or frame id is in
    /// `excluded` (or whose pid is in `excluded_processes`) are composited out
    /// when a compositor redirects them; otherwise the plain root grab is used.
    pub(crate) fn capture_region_rgba(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        excluded_windows: &[u32],
        excluded_processes: &[i32],
        output_rgba: bool,
    ) -> CaptureResult<(Vec<u8>, bool)> {
        if width == 0 || height == 0 {
            return Err(CaptureError::InvalidConfig(
                "capture region must be non-empty".into(),
            ));
        }
        let composed = self
            .capture_composed(x, y, width, height, excluded_windows, excluded_processes)
            .unwrap_or(None);
        let raw = match composed {
            Some(bytes) => bytes,
            None => self.capture_drawable(
                self.root,
                x,
                y,
                width,
                height,
                self.root_depth,
            )?,
        };
        Ok((convert_to_rgba(&raw, width, height, width * 4, output_rgba), false))
    }

    pub(crate) fn capture_window_rgba(
        &mut self,
        window: xproto::Window,
        output_rgba: bool,
    ) -> CaptureResult<(Vec<u8>, u32, u32)> {
        let info = self.window_check(window)?;
        if !info.mapped {
            return Err(CaptureError::InvalidTarget(
                "the window is not viewable".into(),
            ));
        }
        if info.width == 0 || info.height == 0 {
            return Err(CaptureError::InvalidTarget(
                "the window has no capturable pixels".into(),
            ));
        }
        let (drawable, named) = match self.redirected_window_pixmap(info.frame) {
            Some((pixmap, _)) => (pixmap, Some(pixmap)),
            None => (info.frame, None),
        };
        let bytes = self.capture_drawable(drawable, 0, 0, info.width, info.height, 24)?;
        if let Some(pixmap) = named {
            let _ = self.connection.free_pixmap(pixmap);
        }
        let rgba = convert_to_rgba(&bytes, info.width, info.height, info.width * 4, output_rgba);
        Ok((rgba, info.width, info.height))
    }

    fn capture_composed(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        excluded_windows: &[u32],
        excluded_processes: &[i32],
    ) -> CaptureResult<Option<Vec<u8>>> {
        if !self.composite || !self.render {
            return Ok(None);
        }
        let depth = self.root_depth;
        let pixmap = self
            .connection
            .generate_id()
            .map_err(|error| CaptureError::platform(anyhow!("pixmap id: {error}")))?;
        self.connection
            .create_pixmap(depth, pixmap, self.root, width as u16, height as u16)
            .map_err(|error| CaptureError::platform(anyhow!("create pixmap: {error}")))?;
        let gc = self
            .connection
            .generate_id()
            .map_err(|error| CaptureError::platform(anyhow!("gc id: {error}")))?;
        let gc_aux = xproto::CreateGCAux::default().foreground(0);
        self.connection
            .create_gc(gc, pixmap, &gc_aux)
            .map_err(|error| CaptureError::platform(anyhow!("create gc: {error}")))?;
        self.connection
            .poly_fill_rectangle(
                pixmap,
                gc,
                &[xproto::Rectangle {
                    x: 0,
                    y: 0,
                    width: width as u16,
                    height: height as u16,
                }],
            )
            .map_err(|error| CaptureError::platform(anyhow!("clear pixmap: {error}")))?;

        let destination_format = self.format_for_visual(self.root_visual)?;
        let destination_picture = self
            .connection
            .generate_id()
            .map_err(|error| CaptureError::platform(anyhow!("picture id: {error}")))?;
        self.connection
            .render_create_picture(
                destination_picture,
                pixmap,
                destination_format,
                &render::CreatePictureAux::default(),
            )
            .map_err(|error| CaptureError::platform(anyhow!("create picture: {error}")))?;

        let mut excluded = Vec::with_capacity(excluded_windows.len());
        for id in excluded_windows {
            excluded.push(*id);
            if let Some(frame) = self.frame_for(*id) {
                excluded.push(frame);
            }
        }

        let mut composition_ok = true;
        for child in self.top_level_windows()? {
            let info = match self.inspect_window(child) {
                Ok(info) => info,
                Err(_) => continue,
            };
            if !info.mapped || info.width == 0 || info.height == 0 {
                continue;
            }
            if excluded.contains(&child)
                || excluded.contains(&info.window)
                || excluded.contains(&info.frame)
            {
                continue;
            }
            if !excluded_processes.is_empty()
                && excluded_processes.contains(&(info.process_id as i32))
            {
                continue;
            }
            let left = info.x.max(x);
            let top = info.y.max(y);
            let right = (info.x + info.width as i32).min(x + width as i32);
            let bottom = (info.y + info.height as i32).min(y + height as i32);
            if right <= left || bottom <= top {
                continue;
            }
            let Some((named, named_format)) = self.redirected_window_pixmap(info.frame) else {
                composition_ok = false;
                break;
            };
            let source_picture = match self.connection.generate_id() {
                Ok(id) => id,
                Err(_) => {
                    composition_ok = false;
                    break;
                }
            };
            if self
                .connection
                .render_create_picture(
                    source_picture,
                    named,
                    named_format,
                    &render::CreatePictureAux::default(),
                )
                .is_err()
            {
                composition_ok = false;
                break;
            }
            let result = self.connection.render_composite(
                render::PictOp::OVER,
                source_picture,
                0u32,
                destination_picture,
                (left - info.x) as i16,
                (top - info.y) as i16,
                0,
                0,
                (left - x) as i16,
                (top - y) as i16,
                (right - left) as u16,
                (bottom - top) as u16,
            );
            let _ = self.connection.render_free_picture(source_picture);
            let _ = self.connection.free_pixmap(named);
            if result.is_err() {
                composition_ok = false;
                break;
            }
        }

        let _ = self.connection.render_free_picture(destination_picture);
        let _ = self.connection.free_gc(gc);
        if !composition_ok {
            let _ = self.connection.free_pixmap(pixmap);
            return Ok(None);
        }
        let raw = self.capture_drawable(pixmap, 0, 0, width, height, depth);
        let _ = self.connection.free_pixmap(pixmap);
        raw.map(Some)
    }

    fn format_for_visual(&self, visual: xproto::Visualid) -> CaptureResult<render::Pictformat> {
        let formats = self
            .connection
            .render_query_pict_formats()
            .map_err(|error| CaptureError::platform(anyhow!("pict formats: {error}")))?
            .reply()
            .map_err(|error| CaptureError::platform(anyhow!("pict formats reply: {error}")))?;
        if let Some(pict_screen) = formats.screens.get(self.screen_number) {
            for depth in &pict_screen.depths {
                for visual_info in &depth.visuals {
                    if visual_info.visual == visual {
                        return Ok(visual_info.format);
                    }
                }
            }
        }
        Err(CaptureError::BackendUnavailable(
            "no XRender picture format for the root visual".into(),
        ))
    }

    fn format_for_depth(&self, depth: u8) -> CaptureResult<render::Pictformat> {
        let formats = self
            .connection
            .render_query_pict_formats()
            .map_err(|error| CaptureError::platform(anyhow!("pict formats: {error}")))?
            .reply()
            .map_err(|error| CaptureError::platform(anyhow!("pict formats reply: {error}")))?;
        // Prefer ARGB for 32-bit windows, XRGB for 24-bit windows.
        let mut fallback = None;
        for format in &formats.formats {
            if format.depth != depth {
                continue;
            }
            let has_alpha = format.direct.alpha_mask != 0;
            if depth == 32 && has_alpha {
                return Ok(format.id);
            }
            if depth == 24 && !has_alpha {
                return Ok(format.id);
            }
            if fallback.is_none() {
                fallback = Some(format.id);
            }
        }
        fallback.ok_or_else(|| {
            CaptureError::BackendUnavailable("no XRender picture format for the window".into())
        })
    }

    fn redirected_window_pixmap(
        &mut self,
        window: xproto::Window,
    ) -> Option<(xproto::Pixmap, render::Pictformat)> {
        if !self.composite {
            return None;
        }
        let pixmap = self.connection.generate_id().ok()?;
        self.connection
            .composite_name_window_pixmap(window, pixmap)
            .ok()?
            .check()
            .ok()?;
        let geometry = self.connection.get_geometry(window).ok()?.reply().ok()?;
        let format = self.format_for_depth(geometry.depth).ok()?;
        Some((pixmap, format))
    }

    fn capture_drawable(
        &mut self,
        drawable: xproto::Drawable,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        depth: u8,
    ) -> CaptureResult<Vec<u8>> {
        let stride = width as usize * 4;
        let expected = stride * height as usize;
        if self.shm {
            match self.capture_with_shm(drawable, x, y, width, height, expected) {
                Ok(bytes) => return Ok(bytes),
                Err(_) => self.shm = false,
            }
        }
        let reply = self
            .connection
            .get_image(
                ImageFormat::Z_PIXMAP,
                drawable,
                x as i16,
                y as i16,
                width as u16,
                height as u16,
                !0,
            )
            .map_err(|error| CaptureError::platform(anyhow!("get image: {error}")))?
            .reply()
            .map_err(|error| CaptureError::platform(anyhow!("get image reply: {error}")))?;
        let _ = depth;
        if reply.data.len() < expected {
            // Some servers pad rows to a scanline; rebuild tightly packed rows.
            let scanline = (width as usize * 4 + 3) & !3;
            if reply.data.len() < scanline * height as usize {
                return Err(CaptureError::platform(anyhow!(
                    "short image reply ({} bytes for {width}x{height})",
                    reply.data.len()
                )));
            }
            let mut packed = Vec::with_capacity(expected);
            for row in reply.data.chunks_exact(scanline).take(height as usize) {
                packed.extend_from_slice(&row[..stride]);
            }
            return Ok(packed);
        }
        Ok(reply.data[..expected].to_vec())
    }

    fn capture_with_shm(
        &mut self,
        drawable: xproto::Drawable,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        expected: usize,
    ) -> Result<Vec<u8>, ()> {
        let required = expected + 4096;
        if self
            .shm_segment
            .as_ref()
            .is_none_or(|segment| segment.size < required)
        {
            self.shm_segment = Some(self.create_shm_segment(required).map_err(|_| ())?);
        }
        let segment = self.shm_segment.as_ref().ok_or(())?;
        let reply = self
            .connection
            .shm_get_image(
                drawable,
                x as i16,
                y as i16,
                width as u16,
                height as u16,
                !0,
                u8::from(ImageFormat::Z_PIXMAP),
                segment.segment,
                0,
            )
            .map_err(|_| ())?
            .reply()
            .map_err(|_| ())?;
        let size = reply.size as usize;
        if size < expected || size > segment.size {
            return Err(());
        }
        let slice = unsafe { std::slice::from_raw_parts(segment.address, size) };
        Ok(slice[..expected].to_vec())
    }

    fn create_shm_segment(&mut self, size: usize) -> CaptureResult<ShmSegment> {
        let segment = self
            .connection
            .generate_id()
            .map_err(|error| CaptureError::platform(anyhow!("shm id: {error}")))?;
        let reply = self
            .connection
            .shm_create_segment(segment, size as u32, false)
            .map_err(|error| CaptureError::platform(anyhow!("shm create: {error}")))?
            .reply()
            .map_err(|error| CaptureError::platform(anyhow!("shm create reply: {error}")))?;
        use std::os::fd::AsRawFd;
        let address = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                reply.shm_fd.as_raw_fd(),
                0,
            )
        };
        if address == libc::MAP_FAILED {
            return Err(CaptureError::platform(anyhow!("shm mmap failed")));
        }
        Ok(ShmSegment {
            segment,
            address: address.cast::<u8>(),
            size,
        })
    }
}

fn convert_to_rgba(
    raw: &[u8],
    width: u32,
    height: u32,
    stride: u32,
    output_rgba: bool,
) -> Vec<u8> {
    let mut output = vec![0u8; width as usize * height as usize * 4];
    for row in 0..height as usize {
        let source = &raw[row * stride as usize..(row + 1) * stride as usize];
        let target = &mut output[row * width as usize * 4..(row + 1) * width as usize * 4];
        for (src, dst) in source.chunks_exact(4).zip(target.chunks_exact_mut(4)) {
            if output_rgba {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            dst[3] = 255;
        }
    }
    output
}

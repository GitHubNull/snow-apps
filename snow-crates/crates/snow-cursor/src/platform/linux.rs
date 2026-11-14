use x11rb::connection::RequestConnection;
use x11rb::protocol::xfixes::{self, ConnectionExt as _};

use crate::error::CursorCaptureError;
use crate::model::{CursorCompositionMode, CursorShape, CursorShapeCapture};
use crate::sampler::CursorProbe;

pub(crate) struct LinuxCursorSampler {
    connection: x11rb::rust_connection::RustConnection,
}

impl LinuxCursorSampler {
    pub(crate) fn new() -> Result<Self, CursorCaptureError> {
        let (connection, _screen) = x11rb::connect(None)
            .map_err(|error| CursorCaptureError::platform(format!("X11 connect failed: {error}")))?;
        let extension = connection
            .extension_information(xfixes::X11_EXTENSION_NAME)
            .map_err(|error| {
                CursorCaptureError::platform(format!("XFixes query failed: {error}"))
            })?;
        if extension.is_none() {
            return Err(CursorCaptureError::UnsupportedPlatform);
        }
        // GetCursorImage requires XFixes 2.0; servers default to 1.0 until the
        // client negotiates a version.
        let version = connection
            .xfixes_query_version(5, 0)
            .map_err(|error| {
                CursorCaptureError::platform(format!("XFixes version request failed: {error}"))
            })?
            .reply()
            .map_err(|error| {
                CursorCaptureError::platform(format!("XFixes version reply failed: {error}"))
            })?;
        if version.major_version < 2 {
            return Err(CursorCaptureError::UnsupportedPlatform);
        }
        Ok(Self { connection })
    }

    pub(crate) fn sample_cursor(&mut self) -> Result<CursorProbe, CursorCaptureError> {
        let reply = self
            .connection
            .xfixes_get_cursor_image()
            .map_err(|error| {
                CursorCaptureError::platform(format!("XFixes cursor request failed: {error}"))
            })?
            .reply()
            .map_err(|error| {
                CursorCaptureError::platform(format!("XFixes cursor reply failed: {error}"))
            })?;
        let width = u32::from(reply.width);
        let height = u32::from(reply.height);
        let visible = width > 0 && height > 0;
        let shape = if visible {
            CursorShapeCapture::Captured(cursor_shape(
                &reply.cursor_image,
                u32::from(reply.xhot),
                u32::from(reply.yhot),
                width,
                height,
            )?)
        } else {
            CursorShapeCapture::Unavailable
        };
        Ok(CursorProbe {
            x: i32::from(reply.x),
            y: i32::from(reply.y),
            visible,
            shape,
        })
    }
}

fn cursor_shape(
    pixels: &[u32],
    hotspot_x: u32,
    hotspot_y: u32,
    width: u32,
    height: u32,
) -> Result<CursorShape, CursorCaptureError> {
    let expected = width as usize * height as usize;
    if pixels.len() < expected {
        return Err(CursorCaptureError::platform(
            "XFixes returned an incomplete cursor image",
        ));
    }
    let mut rgba = Vec::with_capacity(expected * 4);
    for pixel in &pixels[..expected] {
        // XFixes reports premultiplied ARGB; the compositor expects straight alpha.
        let alpha = (pixel >> 24) & 0xff;
        let straight = |component: u32| -> u8 {
            component
                .checked_mul(255)
                .and_then(|scaled| scaled.checked_add(alpha / 2))
                .and_then(|scaled| scaled.checked_div(alpha))
                .unwrap_or(0)
                .min(255) as u8
        };
        rgba.push(straight((pixel >> 16) & 0xff));
        rgba.push(straight((pixel >> 8) & 0xff));
        rgba.push(straight(pixel & 0xff));
        rgba.push(alpha as u8);
    }
    Ok(CursorShape::from_rgba(
        hotspot_x,
        hotspot_y,
        width,
        height,
        CursorCompositionMode::AlphaBlend,
        rgba,
    ))
}

pub(crate) fn system_crosshair_shape(_width: u32, _height: u32) -> Option<CursorShape> {
    None
}

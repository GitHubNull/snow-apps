//! Shared X11 raw-input observation for the Linux recording effects:
//! XInput2 raw events (clicks, keys, motion) plus XQueryPointer position
//! reads, mirroring the proven M1e gesture backend in the Qt app.
#![cfg(not(any(windows, target_os = "macos")))]

use x11rb::connection::{Connection as _, RequestConnection as _};
use x11rb::protocol::xinput::{self, XIEventMask};
use x11rb::protocol::xproto::{ConnectionExt as _, Keysym, Window};
use x11rb::rust_connection::RustConnection;

use crate::mouse_hook::ObservedMouseButton;

pub struct RawInput {
    pub connection: RustConnection,
    pub root: Window,
}

pub fn connect(mask: XIEventMask) -> Result<RawInput, String> {
    let (connection, screen_index) =
        x11rb::connect(None).map_err(|error| format!("X11 connect failed: {error}"))?;
    let extension = connection
        .extension_information(xinput::X11_EXTENSION_NAME)
        .map_err(|error| format!("XInput2 query failed: {error}"))?;
    if extension.is_none() {
        return Err("XInput2 extension is not available".into());
    }
    // 2.2 (not 2.0): Xephyr withholds raw motion from clients negotiating 2.0.
    let version = xinput::xi_query_version(&connection, 2, 2)
        .map_err(|error| format!("XInput2 version query failed: {error}"))?
        .reply()
        .map_err(|error| format!("XInput2 version reply failed: {error}"))?;
    if version.major_version < 2 {
        return Err(format!(
            "XInput2 2.0 is required, server has {}.{}",
            version.major_version, version.minor_version
        ));
    }
    let root = connection.setup().roots[screen_index].root;
    if let Err(error) = select_raw(&connection, root, mask) {
        // A non-raw selection can conflict with another client's selection on
        // the root window; raw-only still covers clicks and keys.
        let raw_only = mask
            & (XIEventMask::RAW_KEY_PRESS
                | XIEventMask::RAW_KEY_RELEASE
                | XIEventMask::RAW_BUTTON_PRESS
                | XIEventMask::RAW_BUTTON_RELEASE
                | XIEventMask::RAW_MOTION);
        if raw_only == mask {
            return Err(error);
        }
        select_raw(&connection, root, raw_only)?;
    }
    Ok(RawInput { connection, root })
}

fn select_raw(connection: &RustConnection, root: Window, mask: XIEventMask) -> Result<(), String> {
    // XIAllMasterDevices: raw events from all master devices.
    let masks = [xinput::EventMask {
        deviceid: 1,
        mask: vec![mask],
    }];
    xinput::xi_select_events(connection, root, &masks)
        .map_err(|error| format!("XInput2 event selection failed: {error}"))?
        .check()
        .map_err(|error| format!("XInput2 event selection failed: {error}"))
}

pub struct Keymap {
    min_keycode: u8,
    keysyms_per_keycode: u8,
    keysyms: Vec<Keysym>,
}

impl Keymap {
    pub fn load(connection: &RustConnection) -> Result<Self, String> {
        let setup = connection.setup();
        let min_keycode = setup.min_keycode;
        let count = setup.max_keycode - min_keycode + 1;
        let reply = connection
            .get_keyboard_mapping(min_keycode, count)
            .map_err(|error| format!("keyboard mapping query failed: {error}"))?
            .reply()
            .map_err(|error| format!("keyboard mapping reply failed: {error}"))?;
        Ok(Self {
            min_keycode,
            keysyms_per_keycode: reply.keysyms_per_keycode,
            keysyms: reply.keysyms,
        })
    }

    pub fn reload(&mut self, connection: &RustConnection) -> Result<(), String> {
        *self = Self::load(connection)?;
        Ok(())
    }

    pub fn keysym(&self, keycode: u8, column: usize) -> Option<Keysym> {
        if keycode < self.min_keycode || column >= usize::from(self.keysyms_per_keycode) {
            return None;
        }
        let index = usize::from(keycode - self.min_keycode) * usize::from(self.keysyms_per_keycode)
            + column;
        match self.keysyms.get(index) {
            Some(&keysym) if keysym != 0 => Some(keysym),
            _ => None,
        }
    }

    pub fn keycode_vk(&self, keycode: u8) -> Option<u16> {
        self.keysym(keycode, 0).and_then(vk_from_keysym)
    }

    pub fn is_alt_gr_key(&self, keycode: u8) -> bool {
        (0..usize::from(self.keysyms_per_keycode))
            .any(|column| self.keysym(keycode, column) == Some(0xfe03))
    }
}

pub fn pressed_keycodes(connection: &RustConnection) -> Result<Vec<u8>, String> {
    let reply = connection
        .query_keymap()
        .map_err(|error| format!("key state query failed: {error}"))?
        .reply()
        .map_err(|error| format!("key state reply failed: {error}"))?;
    let mut down = Vec::new();
    for (byte_index, byte) in reply.keys.iter().enumerate() {
        for bit in 0..8 {
            if byte & (1 << bit) != 0 {
                down.push((byte_index * 8 + bit) as u8);
            }
        }
    }
    Ok(down)
}

pub fn pointer_position(connection: &RustConnection, root: Window) -> Option<(i32, i32)> {
    let reply = connection.query_pointer(root).ok()?.reply().ok()?;
    reply
        .same_screen
        .then_some((i32::from(reply.root_x), i32::from(reply.root_y)))
}

/// XInput2 button detail to the shared observation model. Wheel buttons are
/// press-only notifications (classic X servers also synthesize a release,
/// which the worker must drop).
pub fn button_from_detail(detail: u32) -> Option<ObservedMouseButton> {
    match detail {
        1 => Some(ObservedMouseButton::Left),
        2 => Some(ObservedMouseButton::Middle),
        3 => Some(ObservedMouseButton::Right),
        4 => Some(ObservedMouseButton::ScrollUp),
        5 => Some(ObservedMouseButton::ScrollDown),
        8 => Some(ObservedMouseButton::Button4),
        9 => Some(ObservedMouseButton::Button5),
        _ => None,
    }
}

/// X keysym to the Windows-VK-flavored key space used by the overlay so
/// chord state and label overrides stay cross-platform. Unknown keysyms are
/// left to the caller's `0x100 + keycode` fallback, like the macOS producer.
pub fn vk_from_keysym(keysym: Keysym) -> Option<u16> {
    let key = match keysym {
        0x61..=0x7a => keysym as u16 - 0x20, // a-z -> A-Z
        0x41..=0x5a => keysym as u16,
        0x30..=0x39 => keysym as u16,
        0xffb0..=0xffb9 => 0x60 + (keysym as u16 - 0xffb0), // KP 0-9
        0xffaa => 0x6a,                                     // KP_Multiply
        0xffab => 0x6b,                                     // KP_Add
        0xffac => 0x6c,                                     // KP_Separator
        0xffad => 0x6d,                                     // KP_Subtract
        0xffae => 0x6e,                                     // KP_Decimal
        0xffaf => 0x6f,                                     // KP_Divide
        0xffbe..=0xffc9 => 0x70 + (keysym as u16 - 0xffbe), // F1-F12
        0xff08 => 0x08,                                     // BackSpace
        0xff09 => 0x09,                                     // Tab
        0xff0d => 0x0d,                                     // Return
        0xff1b => 0x1b,                                     // Escape
        0xff51 => 0x25,                                     // Left
        0xff52 => 0x26,                                     // Up
        0xff53 => 0x27,                                     // Right
        0xff54 => 0x28,                                     // Down
        0xff55 => 0x21,                                     // Prior
        0xff56 => 0x22,                                     // Next
        0xff50 => 0x24,                                     // Home
        0xff57 => 0x23,                                     // End
        0xff63 => 0x2d,                                     // Insert
        0xffff => 0x2e,                                     // Delete
        0xffe1 => 0xa0,                                     // Shift_L
        0xffe2 => 0xa1,                                     // Shift_R
        0xffe3 => 0xa2,                                     // Control_L
        0xffe4 => 0xa3,                                     // Control_R
        0xffe9 => 0xa4,                                     // Alt_L
        0xffea => 0xa5,                                     // Alt_R
        0xffeb => 0x5b,                                     // Super_L
        0xffec => 0x5c,                                     // Super_R
        0xfe03 => 0xa5,                                     // ISO_Level3_Shift
        0x3b => 0xba,                                       // semicolon
        0x3d => 0xbb,                                       // equal
        0x2c => 0xbc,                                       // comma
        0x2d => 0xbd,                                       // minus
        0x2e => 0xbe,                                       // period
        0x2f => 0xbf,                                       // slash
        0x60 => 0xc0,                                       // grave
        0x5b => 0xdb,                                       // bracketleft
        0x5c => 0xdc,                                       // backslash
        0x5d => 0xdd,                                       // bracketright
        0x27 => 0xde,                                       // apostrophe
        _ => return None,
    };
    Some(key)
}

/// Keysym to a display label. Keysyms outside the table convert when they
/// carry an ASCII, Latin-1, or Unicode codepoint.
pub fn label_from_keysym(keysym: Keysym) -> Option<String> {
    let label = match keysym {
        0x20 | 0xff80 => "Space", // space / KP_Space
        0xff08 => "Backspace",
        0xff09 | 0xff89 => "Tab",   // Tab / KP_Tab
        0xff0d | 0xff8d => "Enter", // Return / KP_Enter
        0xff1b => "Esc",
        0xff50 | 0xff95 => "Home",
        0xff57 | 0xff9c => "End",
        0xff55 | 0xff9a => "Page Up",
        0xff56 | 0xff9b => "Page Down",
        0xff51 | 0xff96 => "Left",
        0xff52 | 0xff97 => "Up",
        0xff53 | 0xff98 => "Right",
        0xff54 | 0xff99 => "Down",
        0xff63 | 0xff9e => "Insert",
        0xffff | 0xff9f => "Delete",
        0xfe03 => "AltGr",
        0xffaa => "*",
        0xffab => "+",
        0xffac => ",",
        0xffad => "-",
        0xffae => ".",
        0xffaf => "/",
        0xffbd => "=",
        _ => {
            let codepoint = match keysym {
                0x21..=0x7e => Some(keysym),
                0xa0..=0xff => Some(keysym),
                0x0100_0000..=0x0110_ffff => Some(keysym - 0x0100_0000),
                _ => None,
            }?;
            let character = char::from_u32(codepoint)?;
            return Some(character.to_string());
        }
    };
    Some(label.into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vk_mapping_covers_letters_digits_function_and_modifier_keys() {
        assert_eq!(vk_from_keysym(u32::from(b'a')), Some(0x41));
        assert_eq!(vk_from_keysym(u32::from(b'Z')), Some(0x5a));
        assert_eq!(vk_from_keysym(u32::from(b'5')), Some(0x35));
        assert_eq!(vk_from_keysym(0xffbe), Some(0x70)); // F1
        assert_eq!(vk_from_keysym(0xffc9), Some(0x7b)); // F12
        assert_eq!(vk_from_keysym(0xffb1), Some(0x61)); // KP_1
        assert_eq!(vk_from_keysym(0xff51), Some(0x25)); // Left
        assert_eq!(vk_from_keysym(0xffe1), Some(0xa0)); // Shift_L
        assert_eq!(vk_from_keysym(0xffe4), Some(0xa3)); // Control_R
        assert_eq!(vk_from_keysym(0xffeb), Some(0x5b)); // Super_L
        assert_eq!(vk_from_keysym(0xfe03), Some(0xa5)); // ISO_Level3_Shift
        assert_eq!(vk_from_keysym(0x3b), Some(0xba)); // semicolon
        assert_eq!(vk_from_keysym(0), None);
        assert_eq!(vk_from_keysym(0x1234), None);
    }

    #[test]
    fn labels_cover_named_keys_and_codepoints() {
        assert_eq!(label_from_keysym(0xff0d).as_deref(), Some("Enter"));
        assert_eq!(label_from_keysym(0xff51).as_deref(), Some("Left"));
        assert_eq!(label_from_keysym(0xff8d).as_deref(), Some("Enter")); // KP_Enter
        assert_eq!(label_from_keysym(u32::from(b'!')).as_deref(), Some("!"));
        assert_eq!(label_from_keysym(0xe9).as_deref(), Some("é"));
        assert_eq!(label_from_keysym(0x0100_4e2d).as_deref(), Some("中"));
        assert_eq!(label_from_keysym(0), None);
    }

    #[test]
    fn button_details_map_to_shared_buttons() {
        assert_eq!(button_from_detail(1), Some(ObservedMouseButton::Left));
        assert_eq!(button_from_detail(2), Some(ObservedMouseButton::Middle));
        assert_eq!(button_from_detail(3), Some(ObservedMouseButton::Right));
        assert_eq!(button_from_detail(4), Some(ObservedMouseButton::ScrollUp));
        assert_eq!(button_from_detail(5), Some(ObservedMouseButton::ScrollDown));
        assert_eq!(button_from_detail(8), Some(ObservedMouseButton::Button4));
        assert_eq!(button_from_detail(9), Some(ObservedMouseButton::Button5));
        assert_eq!(button_from_detail(6), None);
        assert_eq!(button_from_detail(12), None);
    }
}

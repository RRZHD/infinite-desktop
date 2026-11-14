# InfiniteDesktop

**English** | [Русский](README.ru.md)

An infinite, zoomable canvas for the Windows 10/11 desktop. Windows live in a virtual
"world" larger than the screen. You move a camera across it, and the real windows
physically move with it, staying full-size and fully interactive. Zooming out opens a
full-screen overview made of live window previews.

## Download

Get `InfiniteDesktop.exe` from the
[latest release](https://github.com/RRZHD/infinite-desktop/releases/latest). There is no
installer: run the file. To quit, use the tray icon or `Ctrl+Alt+Q`.

## How it works

Windows does not let one process scale another process's live window, so there are two
modes:

- **1:1** is for normal work. Panning moves the real windows (`SetWindowPos`).
- **Overview (zoom-out)** is a full-screen view built from live DWM thumbnails of all
  windows. Use it to look around, rearrange windows and jump to a place.

## Controls

All hotkeys are global and can be changed in the settings.

| Action | How |
|---|---|
| **Pan the canvas** | Hold the **middle mouse button** over empty desktop and drag; **two-finger swipe** on a touchpad; horizontal wheel over empty desktop |
| Pan when the desktop is covered by windows | **`Ctrl` + middle button** anywhere, even over an application window |
| **Zoom out** | **Mouse wheel** or **touchpad pinch** over empty desktop (or `Ctrl+wheel`) |
| Pan in steps | `Ctrl+Alt+Arrows` |
| Home (camera to 0,0) | `Ctrl+Alt+H` |
| Show/hide the minimap | `Ctrl+Alt+M` |
| Live previews on the minimap on/off | `Ctrl+Alt+P` |
| Pin/unpin the active window | `Ctrl+Alt+F` (a pinned window stays on screen while the canvas moves) |
| Focus the neighbouring window (left/right/up/down) | `Ctrl+Alt+Shift+Arrows`; the camera follows if the window is not fully on screen |
| Remember a place on the canvas, 1–9 | `Ctrl+Alt+Shift+1…9` (number row or numpad) |
| Go to place 1–9 | `Ctrl+Alt+1…9` |
| Jump to a place | Click the minimap; hold and drag to move the camera continuously |
| Rebuild the window list | `Ctrl+Alt+R` |
| Quit | `Ctrl+Alt+Q` or the tray menu |

## Minimap, tray and settings

The **minimap** shows all windows and a frame for the current screen. It is translucent
and can sit in any corner of the screen.

The **tray icon** opens the settings on left click. Right click opens a menu: minimap,
live previews, rebuild the window list, quit.

The **settings window** opens from the tray icon or from the **⚙** button on the minimap:

- **Hotkeys.** Click a shortcut and press the new one; `Esc` cancels. A shortcut that is
  already taken is rejected with a warning.
- **Canvas bookmarks.** "Remember place 1…9" and "Go to place 1…9" are one row each: you
  set the modifiers, the digit is implied.
- **Minimap corner.**
- **Middle button moves the canvas in any window.** Panning works over any window without
  `Ctrl`. The cost: the middle button no longer reaches applications (orbit in Blender,
  autoscroll in a browser). The taskbar is excluded. Off by default.
- **Maximized windows move with the canvas.** By default maximized windows stay glued to
  their monitor. With this option they move like any other window; on exit they are
  re-maximized on the nearest monitor.
- **Reset to defaults.**

Colours follow Windows: the system accent colour and the light or dark app theme. A
theme change is picked up immediately.

The interface language follows Windows: Russian on a Russian system, English on any other.

## Bringing a window into view

When you switch to a window with **Alt+Tab**, **Win+Tab** or the taskbar and that window
is entirely off screen, the camera moves so the window fits the work area of the monitor
under the cursor. A window that is at least partly visible is left where it is.

## Layout between sessions

- **On start**, windows that have a saved position return to their place on the canvas.
  New or unmatched windows (all of them on the very first start) are laid out in rows
  without overlapping. Matching is by window class and title, falling back to class only.
- **On exit**, the layout is saved and all windows are gathered back onto the visible
  desktop, into the work area of the nearest monitor.

On the first start, windows that were stacked on top of each other spread across the
canvas, so most of them end up outside the monitors. Use panning, zoom or the minimap to
reach them.

## What the app changes while it runs

Both changes are reverted on exit:

- **Aero Snap is turned off.** An infinite desktop has no edges to snap to.
- **Desktop icons are hidden.**

If the app crashes, turn snapping back on in *Settings → System → Multitasking → Snap
windows*. Hidden desktop icons come back after Explorer restarts or after you sign out
and in again.

## Touchpad

Precision touchpad gestures arrive as wheel events. Over empty desktop:

- **Two-finger swipe** pans the canvas in both axes.
- **Pinch** zooms.

Over an application window these gestures are not intercepted, so scrolling and zooming
inside applications work as usual. A mouse wheel is told apart from a touchpad by step
size: a wheel notch zooms, a small touchpad delta pans.

## Zoom (overview)

Scrolling the mouse wheel or pinching over empty desktop zooms the whole desktop out
around the cursor. The background is your wallpaper, dimmed more the further you zoom
out. Previews keep the real stacking order of the windows.

In the overview:

- **Click (left or right) on something inside a window** presses it without leaving the
  overview. Buttons, menus and focus work; after a click the keyboard goes to that window.
- **Click on the background** returns to 1:1, centred on that point.
- **Drag a preview** to move the window on the canvas.
- **Drag the background** or hold the **middle button** to pan the overview.
- **Wheel forward** to 100% leaves the overview.

Windows cannot send input into a scaled-down live window, so clicks are delivered
indirectly: the real window is briefly placed under the cursor behind the overview, a
real click is injected, and the window goes back. Anything that reacts to a click works.
Things that need a continuous cursor inside the application do not: hover, dragging,
and wheel scrolling (the wheel zooms the overview).

## Live wallpapers (Wallpaper Engine and similar)

Live wallpaper programs put their own window inside the desktop window. The app accounts
for that:

- Panning and zooming work over a desktop with a live wallpaper.
- The wallpaper keeps playing in the overview: the background is a live view of the
  desktop, with dimming and window shadows preserved.
- The wallpaper window is never treated as a managed window and is not moved with the
  canvas.

Tested with Wallpaper Engine on Windows 11.

## Under the hood

- **Multiple monitors.** The scene uses virtual-screen coordinates, so windows move
  across the whole workspace of all monitors. Monitor configuration changes are handled.
- **Per-monitor DPI awareness (v2)** for correct coordinates at mixed scale factors.
- **Live previews** on the minimap and in the overview are DWM thumbnails.
- **Direct2D** draws the overview and the settings window.
- **Smooth motion.** Frames are paced by the DWM compositor, all windows move in one
  batch, and only windows on screen are moved every frame.
- **Off-screen windows are parked at the screen edge** with a one-pixel overlap, so
  applications that minimize themselves when they leave all monitors (Photoshop and other
  Adobe apps) keep working.

Settings, layout and bookmarks are stored in `%LOCALAPPDATA%\InfiniteDesktop\`
(`hotkeys.txt`, `ui.txt`, `layout.txt`, `marks.txt`). `wall.log` in the same folder
records how the overview background was obtained and helps diagnose wallpaper problems.

## Limitations

- Windows started as administrator are not moved unless the app also runs as
  administrator (UIPI).
- Minimized windows are not tracked.
- Maximized windows do not move with the canvas unless the option is turned on.
- In the overview, hover, dragging and wheel scrolling inside a window do not work.
- On the minimap, live previews are drawn above the screen frame and can cover it where
  they overlap.

## Ideas

- Panning by pushing the screen edge.
- An exclusion list for windows that should not be managed.
- Separate canvases for different monitors.

## Build

```bat
build.bat
```

Requires Visual Studio Build Tools with the C++ workload and a Windows 10/11 SDK.
`build.bat` finds the toolset (through `vswhere`) and the SDK itself.

## License

[MIT](LICENSE). Anyone may use, modify and distribute the program. It is provided "AS IS",
without warranty of any kind.

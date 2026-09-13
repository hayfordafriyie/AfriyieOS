# Afriyie Shell — UI Design System

**Status:** 📐 designed · 🔨 partially implemented (the v0.1 splash uses the palette and the breakpoint rule)

One design language, two form factors. The same widget tree produces a phone
interface in portrait and a desktop interface in landscape, because splitting
into two UIs is how a cross-platform product becomes two products.

---

## 1. The responsive rule

Evaluated at boot and on every resolution change or display hotplug:

```
aspect_ratio   = width / height
diagonal_inches = sqrt(width² + height²) / ppi

if (aspect_ratio < 1.15 && diagonal_inches < 9.0)   → PHONE MODE
else if (width < 900)                                → COMPACT / TABLET MODE
else                                                 → DESKTOP MODE
```

Aspect ratio alone is not enough — a small landscape window is not a desktop —
so physical size participates. `ppi` is unknown on a bare-metal framebuffer at
v0.1, so the splash uses aspect ratio alone and the full rule arrives with the
compositor at v0.8.

### 1.1 The three modes

| | PHONE | COMPACT | DESKTOP |
| --- | --- | --- | --- |
| Navigation | Full-screen app stack, gesture bar, back swipe | Single window, bottom sheet | Overlapping windows, taskbar, start menu |
| System chrome | Status bar (time, battery), notch-safe insets | Slim status bar | Title bars, tray, window controls |
| Input | Touch: long-press, swipe, pinch | Touch + keyboard | Mouse hover states, right-click, wheel |
| Windows | One at a time | One at a time, resizable | Free-floating, resizable, snappable |
| Type base | 16sp | 15sp | 14px |
| Density | `comfortable` (48px touch targets) | `comfortable` | `compact` … `dense` |
| Settings | Full-screen drill-down | Drill-down | Two-pane sidebar + detail |

Apps write one `build()` function. The framework picks the layout for the current
breakpoint, and only where the layouts genuinely differ does an app branch
explicitly.

---

## 2. Design tokens

A single source of truth, compiled to both C++ `constexpr` values and this
document, so the code and the style guide cannot drift apart.

```haskell
-- ui/theme/tokens.afh
color {
  primary            = #0B6E4F   -- deep green
  onPrimary          = #FFFFFF
  primaryContainer   = #1B4D3E
  secondary          = #F4B400   -- gold
  onSecondary        = #201600
  accent             = #27C08A
  surface            = #121418
  surfaceContainer   = #161A22
  surfaceContainerHi = #1E232E
  onSurface          = #ECEFF4
  onSurfaceVariant   = #9AA3B2
  outline            = #2A3140
  error              = #E53935
  success            = #43A047
}

spacing  { xs = 4, sm = 8, md = 12, lg = 16, xl = 24, xxl = 32, xxxl = 48 }
radius   { sm = 8, md = 12, lg = 16, xl = 28, pill = 9999 }
elevation{ 0 = none, 1 = 1px, 3 = 4px, 6 = 8px, 12 = 16px }

motion {
  micro     = 100ms
  standard  = 200ms
  entrance  = 400ms
  emphasized = cubic-bezier(0.2, 0.0, 0.0, 1.0)   -- emphasized decelerate
  standard_curve = cubic-bezier(0.4, 0.0, 0.2, 1.0)
}

type {
  caption   = 12
  body      = 14
  bodyLarge = 16
  title     = 20
  headline  = 28
}
```

### 2.1 Colour rules

* **Never** encode meaning in colour alone. Status uses an icon or a label too.
* Every foreground/background pair in the token set meets **WCAG AA (4.5:1)** for
  body text and **3:1** for large text and graphical objects.
* The accent colour is for emphasis and focus, not for large fills.
* Dark is the default, not an afterthought. Light mode is a token swap, not a
  separate stylesheet.

### 2.2 Spacing

Everything sits on a **4px base grid**. Values outside the scale are a bug, not a
judgement call — the scale is what makes unrelated screens feel related.

---

## 3. Layout

### 3.1 A declarative tree

```cpp
af::View build() override
{
    return Column{
        .spacing = tokens::spacing::lg,
        .children = {
            Text{"AfriyieOS", style::headline, tokens::color::onSurface},
            Text{"a minimal OS for PCs and phones", style::body,
                 tokens::color::onSurfaceVariant},
            Button{.label = "Settings", .onClick = [this] { open_settings(); }},
        },
    };
}
```

State change → scoped rebuild of the affected subtree → measure/arrange → damage
the changed rectangles only. The tree is retained; the *description* is rebuilt,
which gives the ergonomics of immediate mode with the performance of retained
mode.

### 3.2 The layout engine

Two passes, as in every mature UI toolkit for good reason:

1. **Measure** — each node reports its intrinsic size given the constraints
   coming down from its parent.
2. **Arrange** — each node places its children in the box it was given, then
   damages the region it drew into.

Constraints are `min_w, max_w, min_h, max_h` plus flex weights, so a row can say
"the sidebar is fixed and the content takes the rest" without either knowing the
other's size.

Every node is clipped to its parent. A widget cannot draw outside its box, which
means a layout bug is a visual glitch rather than corruption of a neighbouring
window's pixels.

---

## 4. Rendering pipeline

```
App draws into its own surface (shared memory with the compositor)
        │
        ▼
mark_damage(rect)  → the compositor's dirty-region set
        │
        ▼
Compositor: for each damaged rect, for each window in z-order:
                blit the window's surface, alpha-blend, clip
        │
        ▼
Optional blur / shadow passes (cached, v0.8+)
        │
        ▼
Back buffer --flip--> framebuffer
```

Progression, deliberately staged:

| Version | Rendering |
| --- | --- |
| v0.6 | Full-screen redraw, single buffer, software blit. Correct but slow |
| v0.8 | Dirty rectangles, double buffering, cached shadows |
| v1.2 | Page flipping and hardware-accelerated blits |

**60 FPS with four windows and an animation running** is the v0.8 acceptance
criterion, measured, not assumed.

---

## 5. Motion

Motion exists to explain change, not to decorate.

* **Micro (100 ms)** — a button press, a hover, a toggle.
* **Standard (200 ms)** — a panel expanding, a list item appearing.
* **Entrance (400 ms)** — a window opening, a screen transition.
* **Emphasized decelerate** for anything entering; it arrives quickly and settles.
* Moves over distance take longer than moves over a short distance — a fixed
  duration for both reads as either sluggish or jarring.
* **Animations must be interruptible.** A running animation that cannot respond
  to a new input is worse than no animation at all.
* Nothing blocks on an animation completing. A tap during a transition is
  honoured immediately.

---

## 6. Accessibility

Built in from v0.6, not retrofitted:

* **Full keyboard navigation.** Every interactive element is reachable by Tab and
  activatable by Enter or Space. Focus is always visible.
* **A focus ring that meets contrast requirements** and is never removed without
  an alternative indicator.
* **Minimum touch target of 48×48 px** in phone mode, with 8px of separation.
* **Text scales** to 200% without clipping or overlap.
* **No meaning conveyed by colour alone.**
* **Reduced motion** is respected: entrance animations become instant fades.

---

## 7. Internationalisation

* All strings are UTF-8 end to end — the kernel console already renders `?` for
  anything outside printable ASCII rather than emitting garbage.
* Text measurement and layout are byte-length-independent, so `ɛ` and `ɔ` and
  CJK all lay out correctly.
* Dates and numbers are formatted through locale-aware helpers rather than
  `sprintf`, from the first user-visible date at v1.0.
* Strings live in a resource table from v1.0. Retrofitting i18n into hardcoded
  literals is a mechanical, tedious, large diff; starting with the table costs
  almost nothing.

---

## 8. Component inventory at v1.0

| Category | Components |
| --- | --- |
| Layout | `Column` `Row` `Stack` `Padding` `Center` `Spacer` `ScrollView` `Grid` |
| Content | `Text` `TextInput` `Image` `Icon` `Divider` `ProgressBar` `Spinner` |
| Actions | `Button` `IconButton` `Toggle` `Checkbox` `Radio` `Slider` `Dropdown` |
| Containment | `Card` `Dialog` `Sheet` `Toast` `Tooltip` `ContextMenu` |
| Navigation | `Tabs` `Breadcrumb` `StatusBar` `Taskbar` `AppGrid` `GestureBar` |
| Desktop-only | `TitleBar` `WindowFrame` `ResizeHandle` `TaskSwitcher` |
| Phone-only | `NotificationShade` `LockScreen` `BottomSheet` `GestureHint` |

---

## 9. Implementing an app

Behaviour that must be identical in both modes:

* `build()` — the widget tree for the current breakpoint
* State management and the `setState` rebuild scope
* Every action, without exception

Behaviour that may differ:

* Composition and spacing
* Which navigation chrome is shown
* Whether a control is a floating action button or a toolbar item

If an app has a *feature* that exists on only one form factor, that needs a
reason. "The screen is small" is a layout problem, not a feature difference.

---

## 10. References

* Material Design 3 — the colour-role, elevation and motion model this follows
* Apple Human Interface Guidelines — the phone-side interaction conventions
* Flutter's constraint-based layout model — the measure/arrange design
* [brand.md](brand.md) — the palette and the wordmark
* [../AfriyieOS-Blueprint.md](../AfriyieOS-Blueprint.md) §6 — the summary

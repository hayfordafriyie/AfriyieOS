# Brand

**Status:** ✅ palette and wordmark implemented at v0.1 (the splash screen)

---

## 1. Name and identity

| | |
| --- | --- |
| Name | **AfriyieOS** |
| Always written | `AfriyieOS` — one word, capital A, capital OS |
| Never written | `Afriyie OS`, `afriyieos`, `AFRIYIEOS` (except in code identifiers) |
| v1.0 codename | **Oseadeɛyɔ** |
| Tagline | *a minimal OS for PCs and phones* |

The name is deliberately Ghanaian. The visual identity should read as
contemporary and technical, not as folkloric — the reference is the modern West
African design conversation, not a heritage motif applied to a product.

---

## 2. Palette

Defined once in `kernel/include/afriyie/fb.h` and mirrored in
`docs/design/ui-design-system.md` §2.

| Token | Hex | RGB macro | Use |
| --- | --- | --- | --- |
| `primary` | `#0B6E4F` | `AF_BRAND_PRIMARY` | The mark, the accent strip, primary actions |
| `secondary` | `#F4B400` | `AF_BRAND_SECONDARY` | The crossbar of the mark; sparing emphasis |
| `accent` | `#27C08A` | `AF_BRAND_ACCENT` | Progress, success, focus |
| `surface` | `#121418` | `AF_BRAND_SURFACE` | Base background |
| `surfaceContainer` | `#161A22` | — | Cards, panels |
| `onSurface` | `#ECEFF4` | `AF_BRAND_ON_SURFACE` | Body text on surface |

### 2.1 Why deep green and gold

Green `#0B6E4F` reads as considered and calm, and it survives being rendered at
eight grey levels on a cheap panel — which matters, because a 16bpp framebuffer
is a real target. It also carries meaning without being a national flag
reference, which would make the identity narrower than the project.

Gold `#F4B400` against deep green has a contrast ratio of roughly **7:1**, well
past WCAG AA for text and safely usable for thin graphical elements like the
crossbar of the mark.

The background is a near-black rather than pure black: `#121418` avoids the
smearing that pure black shows on OLED and keeps shadows visible, and it means
`#000000` remains available as a distinct value.

---

## 3. The wordmark

The AfriyieOS mark is a stylised **A**: an outlined triangle with a baseline and
a gold crossbar.

```
        /\
       /  \
      /----\      ← gold crossbar
     /      \
    /________\    ← baseline
```

Drawn from primitives in `kernel/core/splash.c` — a stack of horizontal spans for
the triangle, a filled bar for the crossbar. No image asset, no font dependency,
and it scales to any size because it is generated at the target resolution.

### 3.1 Rules

* **Clear space** of at least half the mark's width on all sides.
* **Minimum size** of 24 px; below that the two-pixel strokes merge and it reads
  as a blob. Use the wordmark's initial `A` in a rounded square instead.
* **Do not** rotate, skew, add a gradient, or place the mark on a busy
  background. On photography or an unclear surface, put it in a
  `surfaceContainer` card first — which is exactly what the desktop splash does.
* **Monochrome use** is permitted in `onSurface` only, and the crossbar then
  becomes the same colour as the triangle.

### 3.2 Together with the wordmark

```
[A mark]  AfriyieOS
          a minimal OS for PCs and phones
```

The mark is vertically centred against the two lines of text. The name is set
larger than the tagline by at least two steps on the type scale, so the hierarchy
survives a glance.

---

## 4. The splash screen

The first thing anyone sees, and the proof that the whole boot path worked. It is
specified as a deliverable, not decoration.

### 4.1 Desktop (landscape)

* Vertical gradient background, `#0A1410` → `#121418`
* A soft accent glow blended at low alpha, not drawn
* A centred card with 16px rounded corners, a drop shadow, and a 4px primary
  accent strip along the top edge
* The mark on the left, the wordmark and tagline to its right
* A progress bar under the text using the accent colour
* A footer line: name, version, codename, resolution, architecture

### 4.2 Phone (portrait)

* Same gradient background and glow
* The mark large and centred, up to 160px
* The wordmark below it, then the version
* A wide progress bar near the bottom
* Nothing else — a phone splash has less room and needs less

### 4.3 Why it looks like this

It reads as a finished product rather than a test pattern, which matters when the
maintainer needs motivation at 2 a.m. in month four. It is also a genuine
integration test: rendering it exercises the framebuffer init, the stride
handling, the colour conversion, gradients, alpha blending, rounded rectangles,
the drop shadow and scaled text. **If the splash is correct, all of those are
correct.**

---

## 5. Voice

* **Direct.** "The memory map is unusable: it describes no free RAM." Not "An
  error has occurred."
* **Specific.** Name the file, the address, the syscall. "kernel/core/vmm.c:412"
  beats "internal error".
* **Non-blaming.** "No usable framebuffer was provided" — not "you configured the
  firmware wrong".
* **Honest about uncertainty.** "This may mean a stale BOOTX64.EFI" beats a
  confident wrong diagnosis.
* **Never cute in an error path.** A joke in a panic message is a joke at the
  expense of somebody who is already having a bad day.

---

## 6. References

* [ui-design-system.md](ui-design-system.md) — tokens, layout, motion
* [`kernel/include/afriyie/fb.h`](../../kernel/include/afriyie/fb.h) — the palette in code
* [`kernel/core/splash.c`](../../kernel/core/splash.c) — the mark and the splash

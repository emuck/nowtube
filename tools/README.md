# Tools

Developer utilities for the nowtube project.

---

## font_convert.py — Google Font → LVGL converter

Fetches a Google Font, converts it to the LVGL `.c` format used by the ESP32
firmware, and places it in `main/fonts/` ready to declare and use.

### Prerequisites

```bash
# Python dependency (just requests for download)
pip3 install requests

# LVGL font converter (Node.js)
npm install -g lv_font_conv
```

### Usage

```bash
# Generate DSEG7 Classic at clock-useful sizes (clock charset = digits + colon)
python3 tools/font_convert.py "DSEG7 Classic" --sizes 40 60 100 120

# Full ASCII for scrolling artist/title text
python3 tools/font_convert.py "Share Tech Mono" --sizes 80 --charset full

# Use a locally downloaded TTF instead of fetching from Google
python3 tools/font_convert.py "MyFont" --sizes 120 --ttf ~/Downloads/MyFont-Regular.ttf

# See recommended fonts for Nextube
python3 tools/font_convert.py --list-fonts
```

### Character sets

| Name | Contents | Use case |
|---|---|---|
| `clock` | 0-9 · : . space ° | Clock digits only (~5KB per size) |
| `clock+` | clock + A M P | Time display with AM/PM suffix |
| `upper` | A-Z · 0-9 · punctuation | Status displays |
| `full` | All printable ASCII | Scrolling artist/title text |

Smaller character sets = smaller `.c` files = faster build and less flash usage.

### After conversion

The tool prints the exact lines you need to add to three files:

```
1. main/CMakeLists.txt   — add the .c file to SRCS
2. main/gui.h            — LV_FONT_DECLARE(font_name_120)
3. your .cpp file        — lv_obj_set_style_text_font(obj, &font_name_120, LV_PART_MAIN)
```

Then `idf.py build` and the font is live.

### Recommended fonts

| Font | Style | Best for |
|---|---|---|
| **DSEG7 Classic** | 7-segment LED | Clock digits — most authentic nixie/LED look |
| **Share Tech Mono** | Clean terminal | Status info, scrolling text |
| **Orbitron** | Geometric futuristic | Matches LightFuture theme aesthetic |
| **Nova Square** | Retro digital | Good alternative to Oswald |
| **Aldrich** | Modern, readable | Small size labels |

All available free on [Google Fonts](https://fonts.google.com).

---

## Future tools (planned)

| Tool | Purpose |
|---|---|
| `theme_preview.py` | Render all digits from an extracted theme as a contact sheet |
| `jpeg_to_spiffs.py` | Resize and optimize JPEG images for SPIFFS upload |
| `led_preview.py` | Preview LED color patterns on the terminal before flashing |

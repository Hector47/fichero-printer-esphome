# fichero-printer-esphome

ESPHome external component to connect a **Fichero D11s** (AiYin) thermal label
printer to an ESP32 over BLE and print 1-bit raster labels from automations or
Home Assistant service calls.

> **Printer specs** – 96 px wide printhead, 203 DPI, prints 1-bit raster images
> onto self-adhesive labels.  Connects via BLE GATT (or Classic Bluetooth SPP on
> supported platforms).

Protocol reverse-engineered from the Fichero APK; full details in the
[Python reference implementation](https://github.com/Hector47/fichero-printer).

---

## Hardware requirements

| Item | Notes |
|------|-------|
| ESP32 board | Any variant; BLE required (ESP32-S3, ESP32-C3 also work) |
| Fichero D11s label printer | Sold as "FICHERO" at Action stores; internally an AiYin D11s |
| Labels | 14 mm × 30 mm self-adhesive (gap-type, default) |

---

## Installation

Add the following to your ESPHome YAML:

```yaml
external_components:
  - source: github://Hector47/fichero-printer-esphome@main
    components: [fichero_printer]
```

---

## Configuration

```yaml
esp32_ble_tracker:          # required – enables BLE scanning/connection

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"   # replace with your printer's address
    id: fichero_ble_client

fichero_printer:
  id: my_printer
  ble_client_id: fichero_ble_client
  density: 1        # 0=light  1=medium (default)  2=thick
  paper_type: 0     # 0=gap/label (default)  1=black mark  2=continuous
```

### Configuration variables

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `ble_client_id` | ID | **required** | ID of the `ble_client` entry for the printer |
| `density` | int 0–2 | `1` | Print darkness: 0 light, 1 medium, 2 thick |
| `paper_type` | int 0–2 | `0` | Label stock: 0 gap, 1 black mark, 2 continuous |

---

## Automation action: `fichero_printer.print`

Triggers a print job with pre-processed 1-bit raster data.

```yaml
fichero_printer.print:
  id: my_printer
  data: <bytes>
```

### Action parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | ID | ID of the `fichero_printer` component |
| `data` | `std::vector<uint8_t>` | Raw 1-bit raster bytes, MSB-first. Length must be a non-zero multiple of **12** (= 96 px ÷ 8). Rows are inferred as `data.size() / 12`. |

### Raster data format

- **96 pixels per row** → 12 bytes per row (MSB first)
- **1 = black** (heater on), **0 = white**
- Total bytes = `rows × 12`
- A standard 30 mm label at 203 DPI is 240 rows → 2 880 bytes

### Generating raster bytes with Python / Pillow

Use the
[`fichero-printer` Python CLI/library](https://github.com/Hector47/fichero-printer)
to turn any image into the right byte array, then embed it in the firmware:

```python
from PIL import Image
from fichero.imaging import prepare_image, image_to_raster

img = Image.open("label.png")
img = prepare_image(img, max_rows=240, dither=True)
raw = image_to_raster(img)          # bytes, len = rows * 12
print(list(raw))                    # paste into ESPHome YAML / lambda
```

### Example – static label in firmware

```yaml
button:
  - platform: template
    name: "Print test bar"
    on_press:
      - fichero_printer.print:
          id: my_printer
          # 8-row solid black bar (96 px × 8 rows)
          data: !lambda |-
            std::vector<uint8_t> d(8 * 12, 0xFF);
            return d;
```

### Example – dynamic data from Home Assistant

Expose a `text` entity that accepts a base64-encoded raster payload, decode it
in a lambda and call the action:

```yaml
text:
  - platform: template
    id: label_b64
    name: "Label raster (base64)"
    mode: text
    optimistic: true

button:
  - platform: template
    name: "Print label"
    on_press:
      - fichero_printer.print:
          id: my_printer
          data: !lambda |-
            // Decode base64 string → raw bytes
            auto encoded = id(label_b64).state;
            return esphome::base64_decode(encoded);
```

---

## Finding your printer's BLE address

1. **Python CLI** (from the reference repo):
   ```bash
   uv run fichero info
   ```
   Look for the `mac_ble` field in the output.

2. **ESP32 serial log** – enable `esp32_ble_tracker` and watch the debug log
   for a device named `FICHERO_XXXX` or `D11s_`.

---

## Complete example

See [`example/fichero_printer.yaml`](example/fichero_printer.yaml) for a
full, ready-to-use configuration.

---

## Print sequence (AiYin D11s)

The component implements the exact sequence verified against D11s firmware 2.4.6:

```
1. 10 FF 10 00 nn     Set density          (100 ms settle)
2. 10 FF 84 nn        Set paper type       ( 50 ms gap)
3. 00 * 12            Wake up              ( 50 ms gap)
4. 10 FF FE 01        Enable (AiYin)       ( 50 ms gap)
5. GS v 0 header      Raster image header  }
   [pixel data…]      1-bit MSB-first      } (500 ms settle after all chunks)
6. 1D 0C              Form feed            (300 ms gap)
7. 10 FF FE 45        Stop (AiYin)         → wait for 0xAA / "OK" (60 s timeout)
```

See [`docs/PROTOCOL.md`](https://github.com/Hector47/fichero-printer/blob/main/docs/PROTOCOL.md)
in the Python repo for the full protocol reference.

---

## License

MIT

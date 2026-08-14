# LEGO 3039px5-Inspired Case Dimensions

Status: preliminary Fusion 360 reference. The LEGO radar graphic diameter is still unknown and must be measured from the physical 3039px5 part before final scaling.

## Reference Scale

LEGO 3039px5 is a standard 2×2 slope part:

```text
Width:  16.0 mm
Depth:  16.0 mm
Slope:  45°
```

Calculate the visual scale from the printed radar circle:

```text
Scale factor = 35.0 ÷ measured LEGO radar-circle diameter
```

Example if the LEGO radar circle measures 10 mm:

```text
Scale factor = 35 ÷ 10 = 3.5
Scaled 16 mm LEGO width = 56 mm
```

Do not scale the entire enclosure blindly from the LEGO brick. The NodeMCU dimensions determine the minimum usable enclosure size.

## Display Geometry

Measured hardware:

```text
Visible GC9A01 glass diameter: 35.0 mm
Glass height above PCB:        2.2 mm
```

Recommended Fusion dimensions:

```text
Display opening diameter:      35.4 mm
Display glass recess:          2.6 mm deep
GC9A01 PCB pocket diameter:    actual PCB diameter + 1.0 mm
PCB pocket depth:              actual PCB thickness + 0.4 mm
Front bezel width:             2.0–2.5 mm
Glass position:                0.3–0.6 mm behind bezel
```

If the PCB measures 40 mm:

```text
PCB pocket diameter:           41.0 mm
```

## Recommended Enclosure Size

The NodeMCU is approximately:

```text
Length: 58 mm
Width:  31 mm
Height: 12–13 mm
```

Use these internal clearances:

```text
NodeMCU bay length:             61 mm
NodeMCU bay width:              34 mm
NodeMCU bay height:             16 mm
```

Recommended outer case starting point:

```text
Width:                          68 mm
Depth:                          72 mm
Height:                         40 mm
Minimum wall thickness:         2.0 mm
Target wall thickness:          2.4 mm
Front slope:                    45°
```

## Front Layout

The front face should preserve the 3039px5 visual arrangement:

```text
          ┌──────────────┐
          │   35 mm      │
          │ radar glass  │
          │              │
          │      38      │
          │   ●      ●   │
          └──────────────┘
```

Suggested dimensions:

```text
Outer display bezel:            39–40 mm diameter
38 label width:                 9–11 mm
38 label height:                5–7 mm
Button diameter:                7–8 mm
Button center spacing:          13–15 mm
Button clearance diameter:      8.0–8.5 mm
```

Place the buttons below the `38` label, with at least 5 mm between the button openings and the lower body edge. Use 0.3–0.4 mm radial clearance around each button cap for an FDM print.

## Rear USB Access

The ESP8266 USB connector exits through the rear wall:

```text
Opening width:                  16–18 mm
Opening height:                 9–10 mm
Clearance around connector:     1.5–2.0 mm
```

Confirm the connector position from the actual NodeMCU before finalizing the rear wall.

Optional cable relief:

```text
Width:                          22 mm
Height:                         3 mm
Radius:                         1.5 mm
```

## Internal Mounting

Use screwless retention:

```text
NodeMCU side rail height:       4–5 mm
NodeMCU rail clearance:         0.3–0.5 mm
Display pocket radial gap:      0.5 mm
Display rear cable clearance:   4–6 mm
```

Use two flexible NodeMCU rails along the long edges and a stop at the USB-opposite end to prevent board movement.

## Scale Reference Table

| LEGO radar diameter | Scale factor | 16 mm LEGO width becomes |
|---:|---:|---:|
| 9 mm | 3.889× | 62.2 mm |
| 10 mm | 3.500× | 56.0 mm |
| 11 mm | 3.182× | 50.9 mm |

The enclosure will likely remain approximately 68×72 mm because the ESP8266 requires more space than the scaled LEGO geometry alone.

## Unresolved Measurement

Measure the diameter of the printed radar circle on the actual LEGO 3039px5 piece. That value determines the final visual scale for the case face, `38` label, button spacing, bezel proportions, and slope layout.

# Micro PPI 38 enclosure

`micro_ppi_case.scad` is a parametric, screwless enclosure inspired by the LEGO 3039px5 radar slope.

## Current design

- 35 mm measured GC9A01 glass
- Parameterized GC9A01 PCB diameter, default 40 mm
- Sloped radar face
- Separate two-button caps below the raised `38` badge
- ESP8266 NodeMCU internal retention rails
- Rear USB opening
- No screws

## Before printing

Measure the round GC9A01 PCB diameter and update `pcb_d` near the top of the SCAD file. Modules vary even when the glass diameter is identical.

Open the file in OpenSCAD, set `pcb_d`, render with F6, and export STL. Print the shell with the sloped face upward. Print button caps and the badge separately.

The first print is a fit prototype. Confirm the display pocket, NodeMCU USB alignment, button travel, and cable clearance before producing the final case.

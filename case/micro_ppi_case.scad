// Micro PPI 38 case
// 3039px5-inspired sloped radar enclosure
// Units: millimeters
//
// Print orientation: front shell with the sloped face upward.
// The model is intentionally parameterized for different GC9A01 modules.

$fn = 96;

// ---------- Hardware ----------
glass_d       = 35.0;   // measured visible GC9A01 glass diameter
pcb_d         = 40.0;   // CHANGE after measuring the actual round PCB
pcb_lip      = 1.0;    // radial clearance around the PCB
pcb_thick     = 1.6;
glass_height  = 2.2;    // measured PCB top to glass top
nodemcu_w     = 31.0;
nodemcu_l     = 58.0;
nodemcu_h     = 12.0;

// ---------- Case ----------
case_w        = 72.0;
case_d        = 54.0;
case_h        = 42.0;
wall          = 2.2;
front_low     = 9.0;
front_slope   = 0.72;   // rise per mm from front toward rear
bezel         = 2.0;

// ---------- Controls ----------
button_d      = 7.0;
button_gap    = 13.0;
button_y      = 9.0;
button_z      = front_low + front_slope * button_y + 8.0;
label_y       = 20.0;
label_z       = front_low + front_slope * label_y + 6.0;

// ---------- Derived ----------
front_angle  = atan(front_slope);
normal_y     = -sin(front_angle);
normal_z     =  cos(front_angle);

// A wedge with a 45-degree-brick visual language and a taller rear.
module outer_wedge() {
    polyhedron(
        points = [
            [0, 0, front_low], [case_w, 0, front_low],
            [0, case_d, front_low + front_slope * case_d],
            [case_w, case_d, front_low + front_slope * case_d],
            [0, 0, case_h], [case_w, 0, case_h],
            [0, case_d, case_h], [case_w, case_d, case_h]
        ],
        faces = [
            [0,1,3,2], [4,6,7,5], [0,4,5,1],
            [2,3,7,6], [0,2,6,4], [1,5,7,3]
        ]
    );
}

// Remove the lower cavity while retaining a sloped front shell.
module inner_cavity() {
    translate([wall, wall + 1, wall])
        cube([case_w - 2*wall, case_d - 2*wall, case_h]);
}

// Cylinder normal to the sloped front plane.
module sloped_cylinder(diameter, depth, x, y, z, overshoot = 4) {
    translate([x, y + normal_y * overshoot, z + normal_z * overshoot])
        rotate([front_angle, 0, 0])
            cylinder(d=diameter, h=depth + 2*overshoot, center=true);
}

// Front shell, circular display aperture, two button apertures, and rear USB opening.
module front_shell() {
    difference() {
        outer_wedge();
        inner_cavity();

        // Display aperture. The larger PCB pocket is shallow; the glass aperture
        // continues through the front bezel.
        sloped_cylinder(pcb_d + 2*pcb_lip, 5,
                        case_w/2, 20,
                        front_low + front_slope*20 + 19);
        sloped_cylinder(glass_d + 2*bezel, 8,
                        case_w/2, 20,
                        front_low + front_slope*20 + 19);

        // Two front button wells below the 38 label.
        for (x = [case_w/2 - button_gap/2, case_w/2 + button_gap/2])
            sloped_cylinder(button_d + 1.0, 7, x, button_y, button_z);

        // USB opening on the rear wall for the NodeMCU connector.
        translate([case_w/2 - 8, case_d - wall - 1, 13])
            cube([16, wall + 4, 9], center=false);
    }
}

// Internal rails hold the NodeMCU without screws.
module nodemcu_rails() {
    rail_h = 4;
    rail_w = 2;
    translate([(case_w - nodemcu_w)/2 - rail_w, 9, wall])
        cube([rail_w, nodemcu_l, rail_h]);
    translate([(case_w + nodemcu_w)/2, 9, wall])
        cube([rail_w, nodemcu_l, rail_h]);
}

// Flexible snap fingers on the rear cover.
module snap_fingers() {
    for (x = [10, case_w - 12])
        translate([x, case_d - 4, 4])
            cube([2, 5, 12]);
}

// Button caps, printed separately in the same file when `show_parts` is true.
module button_caps() {
    for (x = [case_w/2 - button_gap/2, case_w/2 + button_gap/2])
        translate([x, button_y, button_z + 1.5])
            rotate([front_angle, 0, 0])
                cylinder(d=button_d - 0.4, h=2.0);
}

// Raised 38 badge. Print as a separate insert or emboss it in the slicer.
module badge_38() {
    translate([case_w/2, label_y, label_z])
        rotate([front_angle, 0, 0])
            linear_extrude(height=0.8)
                text("38", size=7, halign="center", valign="center",
                     font="Liberation Sans:style=Bold");
}

// ---------- Layout ----------
// Print the shell upright. Separate caps and badge are shown beside it.
front_shell();
nodemcu_rails();

translate([case_w + 12, 12, 0]) button_caps();
translate([case_w + 12, 28, 0]) badge_38();

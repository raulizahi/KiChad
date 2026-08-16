# KiChad Design Script

KiChad Design Script (KDS) is the deterministic source language for a complete KiChad project.  A
script is a reusable `project.kicad_kds` sidecar stored beside `project.kicad_pro`,
`project.kicad_sch`, and `project.kicad_pcb`.  The sidecar is source; ordinary KiCad files and
fabrication packages are compiler outputs.

KDS uses s-expression syntax because it is compact for Codex, reviewable in version control, and
structurally compatible with KiCad's native formats.  It is not Scheme and cannot execute host
code, load plugins, call a shell, access the network, or drive the GUI.  Every form is parsed and
bounded; unknown top-level, board, check, and output kinds are rejected before a program can be
saved or dispatched to a KiChad compiler pass.

## File contract

- Extension: `.kicad_kds`
- Root expression: `kichad_design`
- Required version form: `(version 1)`
- Encoding: UTF-8
- Maximum source size: 16 MiB, including bounded self-contained image payloads
- Maximum nesting: 256 expressions
- Save behavior: compile first, then snapshot-backed atomic replacement
- Concurrency: replacing an existing sidecar requires its current SHA-256 digest
- Compatibility: unknown top-level forms are errors; newer syntax requires a new language version

The project manager recognizes the extension, displays sidecars in the project tree, and opens them
as text. The native `design` tool supports `describe`, paged `context`, exact `read`, inline or
file-backed `compile`, read-only `preview`, `save`, and snapshot-gated `apply`. Read returns the
original bounded UTF-8 source plus its revision metadata. `context` compiles that same source into
a bounded, queryable semantic inventory across project, library, schematic, PCB, and manufacturing
domains; base64 and oversized text payloads are replaced with size/digest metadata, and page output
is capped before it reaches the model. This is ephemeral compiler introspection, not an authored
projection. Loading a sidecar never
rewrites it. Saving preserves the supplied source byte-for-byte after the compiler accepts it.
Preview reports KDS logical IDs, deterministic target UUIDs, counts, and unsupported-backend
diagnostics without connecting to or changing the PCB Editor. Internal compiler IR and KiCad
protobuf payloads are not exposed as a second design representation.

KDS itself is the authoritative AI context and the only external design representation. Its names are explicit,
physical values retain readable engineering units, references resolve locally, and every generated
object has a stable authored logical ID. A model first queries semantic `context`, then reads,
reviews, edits, imports, and exports this same exact source; it never needs to reconstruct design
intent from KiCad serialization or treat compiler JSON as another design file.

Project title-block metadata has one canonical representation inside `project`. `title`, `company`,
`revision`, and `date` may occur once; KiCad's nine comment slots use the indexed
`(comment 1..9 TEXT)` form and each index may occur once. Declaring any of these fields makes the
normalized block explicit: missing fields and comment slots are empty. Apply losslessly installs
the complete block in the root schematic, updates the open board through KiCad's typed API,
verifies native readback, and restores both targets after any later apply failure. Omitting every
metadata field preserves compatibility behavior and does not claim title-block ownership.

Project text variables and schematic field-name templates also have exactly one representation,
nested in `project`:

```scheme
(project sensor_node
  (text_variables
    (variable PRODUCT_NAME "Production Sensor Node")
    (variable DOCUMENT_OWNER "Example Engineering"))
  (field_templates
    (field "Manufacturer Part Number" (visible true) (url false))
    (field "Compliance URL" (visible false) (url true))))
```

Each container may occur once. Its presence means KDS owns and replaces the complete corresponding
project set; an empty `(text_variables)` or `(field_templates)` explicitly clears that set. Omitting
a container leaves the existing project setting unmanaged. Text-variable names follow KiCad's
native character policy and are unique; each value is bounded to 4096 bytes. Template names are
trimmed, case-insensitively unique, and cannot conflict with mandatory symbol fields. Every template
states both its default visibility and URL/browse intent, so there are no implicit per-template
defaults.

Apply inventories both native sets through the typed project API before mutation, journals them,
performs a complete replacement, and requires exact readback. A field-template API added by KiChad
updates the project setting through KiCad's native project serializer and synchronizes an open
Schematic Editor's cached template manager before save. The API checks the serializer's real result
and restores both memory and disk state after a rejected save. Text-variable, field-template, and
net-class change notifications are independent, preventing an unrelated settings update from
rewriting another cached set. Any later apply failure restores the exact prior values in reverse
transaction order.

## Version 1 source model

```scheme
(kichad_design
  (version 1)
  (project sensor_node
    (title "Production Sensor Node")
    (company "Example Engineering")
    (revision "A")
    (date "2026-07-19")
    (comment 1 "Production release")
    (comment 9 "AI-authored KDS")
    (text_variables
      (variable PRODUCT_NAME "Production Sensor Node")
      (variable DOCUMENT_OWNER "Example Engineering"))
    (field_templates
      (field "Manufacturer Part Number" (visible true) (url false))
      (field "Compliance URL" (visible false) (url true))))
  (units mm)

  (sheet root
    (parent none)
    (file "sensor_node.kicad_sch")
    (title "Main"))
  (sheet power
    (parent root)
    (file "power.kicad_sch")
    (title "Power")
    (at 20mm 30mm)
    (size 40mm 20mm)
    (pin VIN input (at 20mm 35mm) (side left))
    (pin LED_A output (at 60mm 35mm) (side right)))

  (library symbol ProductSymbols (table project)
    (uri "${KIPRJMOD}/libraries/ProductSymbols.kicad_sym"))
  (library footprint ProductFootprints (table project)
    (uri "${KIPRJMOD}/libraries/ProductFootprints.pretty"))

  (component R1
    (symbol "ProductSymbols:R")
    (value "10k")
    (footprint "ProductFootprints:R_0603_1608Metric")
    (property "Tolerance" "1%")
    (unit 1
      (sheet root)
      (at 40mm 40mm)
      (rotation 0deg)
      (mirror none)))
  (component LED1
    (symbol "ProductSymbols:LED")
    (value "GREEN")
    (footprint "ProductFootprints:LED_0603_1608Metric")
    (unit 1
      (sheet power)
      (at 40mm 40mm)
      (rotation 90deg)
      (mirror x)))
  (net LED_A (pin R1 1 1) (pin LED1 1 1))
  (no_connect LED1 1 2)
  (wire led-stub
    (sheet power)
    (from 40mm 40mm)
    (to 50mm 40mm)
    (stroke default default))
  (junction led-branch
    (sheet power)
    (at 50mm 40mm)
    (diameter auto)
    (color default))

  (board
    (stackup
      (finish "ENIG")
      (impedance_controlled false)
      (edge_connector none)
      (edge_plating false)
      (layers
        (copper F.Cu (thickness 35um))
        (dielectric core (thickness 1.53mm) (material "FR4")
          (epsilon_r 4.5) (loss_tangent 0.02) (locked false))
        (copper B.Cu (thickness 35um))))
    (outline
      (rectangle board-edge (start 0mm 0mm) (end 40mm 30mm)
        (radius 0mm) (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
    (place R1 (at 10mm 10mm) (rotation 0deg) (side front))
    (route LED_A (id led-a-trace) (from 10mm 10mm) (to 20mm 10mm)
      (width 0.25mm) (layer F.Cu))
    (via LED_A (id led-a-via) (at 20mm 10mm) (diameter 0.8mm) (drill 0.4mm)
      (protection
        (tenting (front open) (back tented))
        (plugging (front plugged) (back inherit))
        (filling filled) (capping uncapped)))
    (zone LED_A
      (id led-a-plane)
      (name "LED copper plane")
      (layers F.Cu)
      (outline
        (polygon
          (point 1mm 1mm) (point 39mm 1mm) (point 39mm 29mm) (point 1mm 29mm)))
      (clearance 0.2mm)
      (min_thickness 0.25mm)
      (connection thermal (thermal_gap 0.3mm) (thermal_spoke_width 0.35mm))
      (islands remove_below (minimum_area 1mm2))
      (fill solid)
      (border diagonal_edge (pitch 0.5mm))))

  (rules
    (minimum_clearance 0.2mm)
    (minimum_connection_width 0.15mm)
    (minimum_track_width 0.18mm)
    (minimum_via_annular_width 0.1mm)
    (minimum_via_diameter 0.6mm)
    (minimum_through_hole_diameter 0.3mm)
    (minimum_microvia_diameter 0.3mm)
    (minimum_microvia_drill 0.1mm)
    (minimum_hole_to_hole 0.25mm)
    (minimum_copper_to_hole_clearance 0.25mm)
    (minimum_silkscreen_clearance 0mm)
    (minimum_groove_width 0.3mm)
    (minimum_resolved_spokes 3)
    (minimum_silkscreen_text_height 0.8mm)
    (minimum_silkscreen_text_thickness 0.08mm)
    (minimum_copper_to_edge_clearance 0.5mm)
    (use_height_for_length_calculations true)
    (maximum_error 0.005mm)
    (allow_fillets_outside_zone_outline false))
  (net_classes
    (class Default
      (clearance 0.2mm) (track_width 0.2mm)
      (via_diameter 0.6mm) (via_drill 0.3mm)
      (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
      (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
      (tuning_profile none) (pcb_color default)
      (wire_width 0.15mm) (bus_width 0.3mm)
      (schematic_color default) (line_style solid))
    (class LED_SIGNAL
      (clearance inherit) (track_width 0.25mm)
      (via_diameter inherit) (via_drill inherit)
      (microvia_diameter inherit) (microvia_drill inherit)
      (diff_pair_width inherit) (diff_pair_gap inherit) (diff_pair_via_gap inherit)
      (tuning_profile inherit) (pcb_color "#22AA44")
      (wire_width inherit) (bus_width inherit)
      (schematic_color inherit) (line_style inherit))
    (assign (pattern LED_A) (classes LED_SIGNAL)))
  (custom_rules
    (rule led_clearance
      (condition "A.NetName == 'LED_A'")
      (layer F.Cu)
      (severity error)
      (constraint clearance (min 0.25mm))
      (constraint track_width (min 0.2mm) (opt 0.25mm) (max 0.5mm)))
    (rule dangling_vias
      (condition always)
      (layer all)
      (severity warning)
      (constraint via_dangling)))
  (source R1
    (manufacturer "Yageo")
    (mpn "RC0603FR-0710KL")
    (datasheet "https://yageogroup.com/content/datasheet/asset/file/PYU-RC_GROUP_51_ROHS_L")
    (lifecycle active)
    (supplier "DigiKey")
    (sku "311-10.0KHRCT-ND")
    (product_url "https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729827")
    (available 6196602)
    (verified_on 2026-07-19)
    (quantity 1))
  (check erc)
  (check drc)
  (check layout)
  (check sourcing)
  (output gerbers)
  (output drill)
  (output bom))
```

The compiler normalizes source into a validated intermediate representation. Logical component and
net identities remain stable across recompilation. Previewed board items use stable RFC 9562 UUIDv8
identities derived from the language namespace, project name, item kind, and authored logical ID;
formatting and statement order do not affect those identities.

Physical board quantities always carry explicit units (`mm`, `mil`, `um`, `nm`, or `in`), area
thresholds use the corresponding square units (`mm2`, `mil2`, `um2`, `nm2`, or `in2`), and
rotations carry `deg`. Generated items such as outlines, traces, arcs, vias, zones, keepouts, and
board text require logical `id` fields. Those IDs are stable across formatting and statement
reordering and are the source identity used by transactional backends; component placement uses the
already-unique reference.

### Electrical qualification and simulation

KDS keeps electrical intent beside the circuit it qualifies. Static contracts cover rail current
budgets and reserve, component voltage/current/power derating, junction-temperature estimates, and
logic-level compatibility. A simulation is another declaration inside that same `electrical` form,
not a second netlist or an opaque SPICE escape hatch:

```scheme
(electrical
  (rail logic_5v
    (net +5V) (voltage 4.75V 5V 5.25V)
    (source_current 500mA) (reserve 20%)
    (load U1 30mA) (load A1 20mA))
  (rating C1
    (operating_voltage 12.6V) (rated_voltage 50V) (derating 80%))
  (thermal A1
    (dissipation 1W) (theta_ja 30C/W)
    (ambient 50C) (maximum_junction 150C))
  (logic step_signal (net STEP)
    (driver U1 1 5) (receiver A1 1 15)
    (output_low 0.4V) (output_high 4.2V)
    (input_low 1.5V) (input_high 3V)
    (signal_min 0V) (signal_max 5V)
    (absolute_min 0V) (absolute_max 5.5V))
  (simulation divider_dc
    (ground GND)
    (device upper (component R1) (unit 1) (kind resistor)
      (nodes VIN OUT) (pins 1 2) (value 10kohm))
    (device lower (component R2) (unit 1) (kind resistor)
      (nodes OUT GND) (pins 1 2) (value 10kohm))
    (source supply (kind voltage) (positive VIN) (negative GND) (dc 5V))
    (analysis nominal (kind operating_point))
    (assert output_voltage (analysis nominal) (probe voltage OUT)
      (minimum 2.49V) (maximum 2.51V) (scope all))))
(check electrical)
```

Simulation devices explicitly bind an ordered component unit/pin list to an ordered declared-net
list; compilation rejects any mapping that disagrees with schematic connectivity. Supported primitives
are resistors, capacitors, inductors, diodes, BJTs, and MOSFETs; semiconductor models are finite,
typed diode/NPN/PNP/NMOS/PMOS parameter maps. Voltage and current sources support DC, AC, and pulse
stimulus. Analyses are operating point, transient, DC sweep, and AC sweep. Assertions probe node or
differential voltage and source current, with typed minimum/maximum bounds evaluated over every
sample or the final sample.

`verify` operation `electrical` evaluates the deterministic static contracts and runs every declared
analysis with the installed `ngspice` executable through generated, bounded temporary netlists. It
returns the numerical assertion extrema and complete pageable issues; arbitrary SPICE directives,
includes, paths, shell text, and host code are never accepted. A physical design containing
components or nets must declare `(check electrical)` and a non-empty electrical contract before the
fabrication plan can be production-ready. Simulation is used where an adequate component model
exists; modules and digital systems can instead be qualified by explicit rail, rating, thermal,
logic, firmware, and bring-up contracts. A truly empty mechanical board has no electrical gate to
run.

### Sourcing evidence

KDS has one sourcing representation: at most one `(source REF ...)` form for each component. There
is no generated sourcing JSON, database, or second model-context projection. Codex uses its forced
live web search before selecting or retaining a component, then records the evidence directly in
the same sidecar that it reads, edits, exports, and imports with the project.

A production-complete record for every footprint-bearing component contains:

```scheme
(source U1
  (manufacturer "Manufacturer name")
  (mpn "Exact manufacturer part number")
  (datasheet "https://manufacturer.example/part.pdf")
  (lifecycle active)
  (supplier "Distributor name")
  (sku "Exact distributor SKU")
  (product_url "https://distributor.example/part")
  (available 1234)
  (verified_on 2026-07-19)
  (quantity 1)
  (unit_price "1.23 USD")
  (notes "Optional bounded evidence note"))
```

`datasheet` and `product_url` must be HTTPS URLs. `lifecycle` is `active`, `nrnd`,
`last_time_buy`, or `obsolete`; only `active` passes cleanly. `available` is the non-negative stock
reported by the named supplier, `quantity` is the positive design quantity, and `verified_on` is a
real `YYYY-MM-DD` calendar date. `unit_price` and `notes` are optional; the identity, URLs,
lifecycle, supplier/SKU, stock, date, and quantity are required by the production gate. A
footprintless virtual or power component does not require a distributor record.

The native `verify` tool's `sourcing` operation compiles the exact `.kicad_kds` source and returns
complete error/warning counts plus bounded pageable issues such as `missing_source`,
`missing_evidence_field`, `non_active_lifecycle`, `not_orderable`, `future_evidence`, and
`stale_evidence`. Evidence is seven days old at most by default; callers may explicitly select one
through ninety days with `maxAgeDays`. The gate checks the cached facts deterministically and does
not silently access the network or claim that a saved URL still has the same content. Codex must
refresh the form with live web search before treating stale evidence as production-ready.

### Fabrication export

KDS output declarations feed one production implementation profile,
`kichad-production-10.0.4-v17`; there is no second job-file or output-profile representation. A
production-ready plan requires all of the following declarations in the same sidecar:

```scheme
(check erc)
(check electrical)
(check drc)
(check layout)
(check sourcing)
(check fabrication)
(output gerbers)
(output drill)
(output ipcd356)
(output pick_place)
(output bom)
; optional: (output step)
; optional: (output stepz)
; optional: (output brep)
; optional: (output glb)
; optional: (output stl)
; optional: (output u3d)
; optional: (output xao)
; optional: (output 3d_pdf)
; optional: (output pdf)
; optional: (output board_ps)
; optional: (output board_render)
; optional: (output schematic_pdf)
; optional: (output schematic_svg)
; optional: (output schematic_dxf)
; optional: (output schematic_ps)
; optional: (output schematic_bom)
; optional: (output legacy_bom_xml)
; optional: (output ipc2581)
; optional: (output odbpp)
; optional: (output netlist)
; optional: (output assembly_svg)
; optional: (output assembly_dxf)
; optional: (output gencad)
; optional: (output vrml)
; optional: (output board_stats)
```

The KDS project name, root schematic stem, and board stem must match, and the explicit KDS stackup
selects every copper, solder-mask, solder-paste, and silkscreen production layer; `Edge.Cuts` is
always added. `fabricate.plan` is read-only. `fabricate.export` additionally requires the exact
compiled KDS SHA-256, a complete pre-turn project snapshot, and visible final confirmation from the
KiChad host. The plan rejects a native board whose enabled plot layers, ordered physical stack,
thicknesses, materials, dielectric properties/locks, finish, impedance, edge-connector, or plating
policy differs from compiled KDS. Only KiCad 10.0.4 board format `20260206` and schematic format
`20260306` are accepted. Every native footprint not marked `board_only` must have a unique reference,
and the exact reference-to-library-ID map must equal the footprint-bearing component inventory in
compiled KDS.

Export copies the required project inputs, project-local `.kicad_sym`/`.kicad_mod` libraries, and
local 3D models into a bounded private snapshot before running the real sibling KiCad CLI. Native
ERC and DRC (including schematic parity) and the KDS sourcing gate must be clean. Exclusions or
ignored checks stop release unless the user explicitly approves waivers and `allowWaivers` is set;
the manifest then records release status `waived` rather than `clean`.

The output is one project-side `fabrication/` directory containing Gerber layer files and a Gerber
job, Excellon drill files plus PDF maps and report, an IPC-D-356 electrical-test netlist, placement
CSV, a BOM derived directly from KDS sourcing forms, optional STEP/PDF files, optional inspectable
IPC-2581C manufacturing XML, an optional validated ODB++ ZIP, and `manifest.json`. Optional
`assembly_svg` and `assembly_dxf` jobs emit separate front/back `F.Fab` and `B.Fab` drawings with
`Edge.Cuts` common to both; their fixed profile sketches pads, hides DNP footprints, uses board-area
page sizing, millimetre DXF units, and verifies exact filenames plus bounded XML/DXF structure.
`gencad` emits unique pin and footprint definitions with stored origin coordinates, `vrml` emits a
millimetre model excluding DNP and unspecified parts, and `board_stats` emits typed millimetre JSON
with board identity, outline, pad/via/component counts, and drill inventory. Each passes a dedicated
format-aware parser before installation. BREP, binary glTF (`glb`), triangular ASCII STL, STEPZ,
U3D, XAO, and interactive `3d_pdf` use the same complete fixed-origin 3D geometry policy as STEP.
Each has a bounded structural validator for its native container and scene/topology data; STEPZ is
inflated with a hard limit, while 3D PDF's embedded U3D stream is decompressed and fully validated.
`board_ps` emits a separate A4 DSC PostScript document for each enabled physical layer, with exact
filename/layer identity and a restricted drawing-only operator profile. The four `schematic_*`
outputs render the guarded root-schematic snapshot as PDF or a bounded set of
per-sheet SVG, DXF, or PostScript drawings. Their validators require KiCad/Eeschema producer
identity, exact root-sheet filenames, complete page/container structure, and no added external
actions, unsafe SVG references, or privileged PostScript operators.
`board_render` runs KiCad's native CPU 3D renderer with a fixed transparent top view and emits one
lossless 1008-by-1008 RGBA PNG. Validation checks the exact filename and image header, every chunk
CRC, ordered IDAT stream, bounded zlib expansion, every reconstructed PNG scanline filter, and
visible varied board pixels before the render can enter the release package.
`schematic_bom` emits an ungrouped five-column native KiCad CSV with Reference, Value, Footprint,
Quantity, and DNP fields. `legacy_bom_xml` emits KiCad's Eeschema `export version="E"` XML for older
BOM consumers. KiChad bounds and parses each representation, requires the exact KiCad 10.0.4
producer/schema, and matches every reference, value, footprint, quantity, and DNP state to the
compiled KDS component inventory.
`netlist` runs the native Eeschema exporter against the same private root-schematic snapshot used by
ERC; KiChad parses the result and requires its exact component-reference and ref/pin connectivity
sets to match compiled KDS intent.

KiChad bounds and validates every artifact. The BOM reference set must equal all footprint-bearing
KDS components, while the
native placement reference set must equal all non-DNP footprint-bearing KDS components; empty,
duplicate, missing, or extra references reject the package. KiChad records exact byte counts and
SHA-256 values and installs the complete directory atomically. A gate, exporter, validation,
stale-input, manifest, or installation failure leaves the previous fabrication directory intact.
Native KiCad creation timestamps are preserved, so separate exports may have different artifact
hashes even when their KDS and native design inputs are equal.

### Firmware, programming, and bring-up

A manufacturable package is not automatically a running product. KDS therefore carries the
firmware and physical bring-up handoff in the same source as the circuit and board:

```scheme
(production
  (assembly
    (acceptance ipc-a-610-class-2)
    (process lead_free_reflow)
    (solder_alloy "SAC305")
    (stencil top)
    (stencil_thickness_um 120)
    (cleaning no_clean)
    (coating none)
    (instruction mcu-orientation
      (scope component U1)
      (text "Install U1 with its pin-one mark aligned to the assembly drawing.")))
  (firmware controller
    (path "firmware/controller.hex")
    (format ihex)
    (sha256 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
    (bytes 32768)
    (version "1.0.0")
    (target U1)
    (device_code
      (toolchain cmake)
      (toolchain_version "3.28.3")
      (target "stm32g0-production")
      (entry "src/main.c")
      (file "src/main.c"
        (language c)
        (sha256 "86004d65c4f387c95467c6cee92bc1f1f8cb04d6650be09fbd1e359834a56766")
        (data_base64 "aW50IG1haW4odm9pZCl7cmV0dXJuIDA7fQo="))))
  (program controller
    (firmware controller)
    (target U1)
    (interface swd)
    (connector J3)
    (device "STM32G0B1CBT6")
    (voltage 3.3)
    (speed_khz 1000)
    (erase chip)
    (reset run)
    (verify true)
    (signal swdio 2)
    (signal swclk 4)
    (signal reset 6)
    (signal ground 3))
  (power main
    (connector J1)
    (positive_pin 1)
    (return_pin 2)
    (voltage 12)
    (current_limit 0.25)
    (settle_ms 250))
  (test input-not-shorted
    (stage unpowered)
    (method resistance)
    (instrument dmm)
    (target net VIN)
    (range 1000 1000000 ohm)
    (after_ms 0)
    (timeout_ms 5000)
    (required true))
  (test rail-3v3
    (stage power_on)
    (method voltage)
    (instrument dmm)
    (target net +3V3)
    (range 3.20 3.40 V)
    (testpoint TP1)
    (power main)
    (after_ms 250)
    (timeout_ms 5000)
    (required true))
  (test firmware-response
    (stage functional)
    (method functional)
    (instrument fixture)
    (target component U1)
    (expected pass)
    (power main)
    (program controller)
    (after_ms 0)
    (timeout_ms 5000)
    (required true)
    (procedure "Require the expected signed firmware response.")))
```

Each firmware image has exactly one source: a confined project-relative regular `path`, or up to
8 MiB of self-contained `(data_base64 DATA)` stored directly in the KDS. Path format and extension
must agree; embedded data is decoded during compilation. The exact byte count and lowercase
SHA-256 are mandatory in both cases. Programming is protocol data rather than a
shell command: it names the firmware, target, physical connector signal map, device, electrical
level, clock, erase/reset behavior, and mandatory readback verification. Power profiles identify
the physical connector pins, supply voltage, hard current limit, and settling time. Every
programming and power pin must exist in the KDS schematic connectivity and every firmware target,
programming target/connector, test point, and component-scoped assembly instruction must resolve to
a physical footprint. Protocols also require their actual semantic signal set: SWD requires
`swdio/swclk/gnd`, JTAG requires `tms/tck/tdi/tdo/gnd`, ISP requires
`mosi/miso/sck/reset/vcc/gnd`, and the UPDI, PDI, debugWIRE, UART bootloader, USB DFU, and CAN
bootloader adapters have corresponding checked signal sets. Two logical signals cannot silently
share one connector pin.

Optional `device_code` keeps maintainable firmware source in the same authored representation.
It names a supported trusted-adapter family (`arduino_cli`, `platformio`, `zephyr_west`, `esp_idf`,
`pico_sdk`, `cmake`, or `cargo_embedded`), its exact version, target, and one bundled entry path.
Each of at most 128 source/configuration files is strict UTF-8, base64 encoded, independently
SHA-256 bound, limited to 1 MiB, and placed under
`fabrication/production/source/FIRMWARE_ID/`. Locked dependencies use
`(dependency NAME VERSION SHA256)`. The complete source bundle is limited to 8 MiB. Generated
`production-plan.json` retains file hashes, sizes, languages, toolchain, target, entry, and
dependency locks but removes base64 payloads, keeping fixture input concise. At this stage the
release package proves source and binary identities but does not claim it reproduced the binary
from source; trusted in-process build adapters remain an explicit capability gap.

The required `assembly` block makes the contract-manufacturer handoff explicit: IPC-A-610
acceptance class, lead-free/leaded/hand/mixed process, solder alloy, stencil sides and thickness,
cleaning, coating, and ordered special instructions scoped either to the whole board or a resolved
component reference. A stencil thickness is required exactly when at least one stencil side is
selected. This process intent is packaged beside the BOM, placement data, and assembly drawings.

Bring-up tests remain in source order. Numeric voltage, current, resistance, and frequency tests
require an accepted range with the matching physical unit. Continuity, logic, visual, and
functional tests require an explicit result; visual and functional checks also require a bounded
procedure. Every release test is required. Targets are semantic nets, components, or component
pins, and an accessible testpoint may be named separately. A running-board contract contains at
least one unpowered test, one current-limited `power_on` test, and one `programmed` or `functional`
acceptance test. Powered tests name one or more ordered supply profiles as
`(power LOGIC MOTOR ...)`; programmed and functional tests likewise
name one or more ordered steps as `(program CONTROLLER SENSOR ...)`, so a fixture never guesses
which complete electrical or firmware state a measurement assumes. The compiler enforces monotonic
`unpowered` → `power_on` →
`programmed` → `functional` ordering; a later line cannot move a production traveler back into an
earlier electrical state.

`fabricate.plan` now reports two distinct facts: `productionReady` means the PCB manufacturing
package has all required checks/outputs, while `runningReady` additionally requires this complete
production handoff. Export snapshots each declared firmware input, validates its size and digest,
and copies it under `fabrication/production/firmware/`. The deterministic
`fabrication/production/production-plan.json` is a compiler artifact for programming fixtures and
test stations; it is not a second authored representation. Firmware and plan digests are recorded
with all KiCad artifacts in `manifest.json`. The same package carries a digest-checked `design/`
tree containing the exact KDS, current board and schematic hierarchy, project tables/settings,
project-local libraries/rules/worksheet, and confined local models; UI preferences and local
history are deliberately excluded. This makes the KDS sidecar and the native files it materialized
portable together. KDS intentionally cannot execute arbitrary commands.
Hardware programmer/test-station execution and signed per-unit result ingestion remain explicit
capability gaps rather than being falsely reported as complete.

### Schematic hierarchy

KDS has one explicit hierarchy representation. Every declared hierarchy has exactly one root sheet;
the root file is exactly `PROJECT.kicad_sch`. Non-root sheets declare their parent, project-relative
`.kicad_sch` file, displayed title, rectangle, and zero or more pins:

```scheme
(sheet root
  (parent none)
  (file "controller.kicad_sch")
  (title "Main"))
(sheet power
  (parent root)
  (file "power.kicad_sch")
  (title "Power")
  (at 20.32mm 30.48mm)
  (size 40.64mm 20.32mm))
```

Pin direction is one of `input`, `output`, `bidirectional`, `tri_state`, or `passive`; side is
`left`, `right`, `top`, or `bottom`. A pin position must lie on its declared rectangle edge. Sheet
IDs, sibling titles, and pin names are unique in their scopes. Parent references must resolve;
cycles, recursive ancestor file reuse, path traversal, absolute paths, case-only file collisions,
duplicate fields, and more than 128 sheets, 256 pins per sheet, or 4096 hierarchy pins are rejected
before inventory or mutation. A single native screen file cannot yet be instantiated by multiple
KDS sheet IDs, so shared-screen aliases are rejected rather than compiled incorrectly.

Sheet pins are optional for ordinary KDS nets. When an automatic, wired, or hierarchical net crosses a
sheet boundary, the planner infers the minimum required interface on every boundary between its
endpoints. It deterministically allocates a passive native sheet pin, matching child hierarchical
label, and readable wire paths on both parent and child pages. An explicit same-name sheet pin is
reused when direction or placement matters. Inferred interfaces prefer 2.54 mm spacing and fall
back to KiCad's 1.27 mm connection grid; a boundary that cannot safely hold the interface fails with
steering to enlarge the sheet or declare buses instead of overlapping pins.

The compiler derives stable UUIDv8 identities for each managed sheet symbol, sheet pin, and child
hierarchical label. Existing screen UUIDs are never replaced: they are inventoried first and used in
the native instance paths. New screen UUIDs, page order, and child-interface label layout are
deterministic compiler output, not another authored representation. Existing schematics are edited
by bounded node spans: unmanaged expressions, fields, ordering, quoting, and UUIDs retain their
exact bytes. Only UUIDs proven in `managedSchematicItems` may be removed.

Every changed schematic is installed atomically with its exact prior presence and bytes in the KDS
apply journal. The complete desired hierarchy is then loaded through the sibling KiCad 10
`kicad-cli sch export netlist` path with a 30-second process deadline. A launch, timeout, parse, or
hierarchy error restores changed schematics, project library tables, and board settings in reverse
order. Existing files and every parent directory must already be project-confined; a missing nested
sheet directory is rejected before mutation.

### Schematic components and connectivity

KDS has one component representation for single- and multi-unit symbols. Component-level fields
declare the reference, exact library symbol, value, footprint, optional custom properties, and DNP
state. Each physical schematic unit is an explicit nested placement:

```scheme
(component U1
  (symbol "ProductSymbols:DUAL_OPAMP")
  (value "OPA2192")
  (footprint "ProductFootprints:VSSOP-10")
  (property "Manufacturer" "Texas Instruments")
  (unit 1 (sheet analog) (at 50mm 40mm) (rotation 0deg) (mirror none))
  (unit 2 (sheet analog) (at 70mm 40mm) (rotation 180deg) (mirror y)))

(net SENSOR_OUT (pin U1 1 1) (pin U1 2 7))
(no_connect U1 2 5)

(component #PWR01
  (symbol "ProductSymbols:GND")
  (value "GND")
  (footprint none)
  (unit 1 (sheet analog) (at 80mm 40mm) (rotation 0deg) (mirror none)))
```

Required and custom field values remain part of that same component. `Datasheet` and `Description`
inherit the resolved library-symbol values unless the component supplies `(datasheet TEXT)` or
`(description TEXT)`. A custom `(property NAME VALUE)` is bounded to a 128-byte name and a
4096-byte value. Because KiCad stores field geometry on each placed symbol unit, an optional
`field` form belongs inside that unit and names the component field it renders:

```scheme
(component R1
  (symbol "ProductSymbols:R")
  (value "10k")
  (footprint "ProductFootprints:R_0603")
  (datasheet "https://example.com/r1.pdf")
  (description "Precision sense resistor")
  (property "Manufacturer Part" "RC0603FR-0710KL")
  (unit 1
    (sheet analog) (at 40mm 40mm) (rotation 0deg) (mirror none)
    (fields_autoplaced false)
    (field Value
      (at 43mm 42mm) (rotation 12.5deg)
      (visible true) (show_name true) (autoplace false)
      (size 1.2mm 1.5mm) (font "DejaVu Sans") (line_spacing 1.25)
      (thickness 0.2mm) (color #11223380) (justify right top)
      (bold true) (italic true)
      (hyperlink "https://example.com/value") (private false))
    (field "Manufacturer Part"
      (at 43mm 44mm) (rotation 0deg)
      (visible false) (show_name false) (autoplace true)
      (size 1mm 1mm) (font stroke) (line_spacing 1)
      (thickness auto) (color default) (justify center center)
      (bold false) (italic false) (hyperlink none) (private true))))
```

Field coordinates are absolute coordinates on the unit's schematic sheet. Every explicit field
layout spells out the complete native formatting state; there is no second style object and no
implicit partial merge. The name must be one of `Reference`, `Value`, `Footprint`, `Datasheet`, or
`Description`, or exactly match a custom property on the component. Only custom fields may be
private. `fields_autoplaced` records KiCad's symbol-level native state and defaults to `true` for
components that do not opt into explicit layouts. A field that has no explicit layout retains
KiChad's deterministic current-format default. Field mirroring is deliberately absent because the
KiCad 10 schematic parser does not retain that token for fields; unit mirroring remains explicit
and lossless. Project field-name templates and project text-variable ownership are separate
capabilities and are not implied by instance-field formatting.

The endpoint tuple is always `(pin REFERENCE UNIT PIN_NUMBER)`; there is no implicit-unit spelling.
Every component in a declared hierarchy has at least one unit placement. Unit numbers are unique
within the component, range from 1 through 256, resolve to the exact library symbol, and declare a
sheet, position, orthogonal rotation (`0deg`, `90deg`, `180deg`, or `270deg`), and mirror policy
(`none`, `x`, `y`, or `xy`). Native required fields are controlled by `symbol`, `value`, and
`footprint`, so custom properties cannot redefine `Reference`, `Value`, `Footprint`, `Datasheet`, or
`Description`. The same component form represents power and other virtual symbols: `(footprint
none)` compiles to KiCad's native empty Footprint property. Such a component cannot be referenced by
a board `place` form. There is no separate power-symbol representation.

Executable symbols currently resolve only from project-local `.kicad_sym` files. KiChad inventories
each file within the project, rejects symlinks and paths that escape the project, bounds individual
files to 16 MiB and the total inventory to 32 MiB, and extracts the exact named symbol through the
lossless parser. The cached native symbol is not synthesized from a placeholder: its original
graphics, fields, pin numbers, and pin coordinates become compiler input. Same-library `extends`
chains are supported to a bounded depth of 64. The resolver detects missing parents and cycles,
inherits the root graphics and pins, and applies KiCad's mandatory-field and custom-field override
rules from root to leaf. It renames inherited units and emits a fully flattened cache symbol because
KiCad forbids inheritance inside schematic `lib_symbols`; unsupported embedded-file overrides are
rejected rather than dropped. Global symbol libraries remain valid dependencies for board-only
programs but are not accepted for executable schematic placement because their contents depend on
the host installation.

The resolver also preserves the exact library symbol's native `exclude_from_sim`, `in_bom`,
`on_board`, and `in_pos_files` semantics on every placed unit. Missing fields use KiCad 10's native
defaults; malformed or duplicate flags abort before planning. This keeps virtual and power symbols
out of downstream artifacts whenever their actual library definition requires it, without adding
KDS-only policy fields.

Each placed unit, pin instance, generated wire segment, net-name anchor, and explicit no-connect
marker receives a stable UUIDv8 identity. A KDS net normally needs only its name and endpoints:

```scheme
(net SENSOR_OUT
  (pin U1 1 1)
  (pin R4 1 1))

(net GND
  (presentation labels)
  (pin U1 1 4)
  (pin C2 1 2)
  (pin J1 1 2))
```

Omitting `presentation` selects `auto`: same-sheet endpoints receive a deterministic physical
wire tree, while cross-sheet endpoints receive the inferred native hierarchy described above.
`hierarchical` explicitly requests that same hierarchy-aware policy. `wired` requests physical
wiring on each involved page and uses the same native sheet-pin interfaces when endpoints cross
sheet boundaries. `labels` is the only
label-only connectivity mode; it deliberately emits short global-label stubs and is appropriate
for intentional global or power connectivity.

Physical routing resolves the rotation- and mirror-transformed native connection point of every named pin
and creates a deterministic orthogonal tree on each involved page. The router honors each pin's outward
direction, uses a single trunk for branches, adds junctions at internal branch points, and unions
touching or overlapping collinear intervals before splitting them at every pin, branch, and name
anchor and assigning stable segment identities. That normalization matches KiCad's own
collinear-wire behavior without allowing a pin to disappear into the interior of a merged segment.
An inferred child hierarchical label is placed on a short clear stub beside a real circuit endpoint
when possible; the fixed page-margin lane is only a fallback. This keeps dense multi-sheet designs
readable without creating long wire walls between a component and its interface. The router reserves
every present and future endpoint, its first 1.27 mm exit corridor, and every emitted segment by net.
It moves trunks, label stubs, and detours away from foreign endpoints and rejects every crossing,
T/corner, or collinear contact between different nets. If the simple direct or dogleg candidates are
blocked, a bounded deterministic orthogonal grid search finds a legal multi-turn path. A genuine
no-path diagnostic identifies the affected sheet, both coordinates, and the exact blocking pin or
wire coordinates. A truly non-planar page falls back to visible labels at its endpoints instead of
silently shorting nets; those labels retain the route's original naming scope, so a same-sheet global
net cannot silently become a sheet-qualified PCB-parity mismatch.
`isolatedLabelFallbacks` reports that decision for review.
One global name anchor appears at a physical route endpoint on exactly one routing page. It
preserves the exact unqualified KDS/PCB net name; it never substitutes for the visible wire or
hierarchy interfaces. A second anchor is neither required nor emitted. Fabrication accepts automatic, wired, and
hierarchical nets as reviewable. A design whose only connectivity uses explicit `labels` remains
blocked from production readiness until physical wire paths are generated and every rendered page
is reviewed. Explicit label presentation across several sheets also produces a bounded compiler
warning because automatic hierarchy is normally clearer.

All electrical placement and drawing anchors are normalized during native lowering to KiCad's
1.27 mm schematic connection grid. Component origins, sheet rectangles and pins, wires, buses, bus
entries, junctions, labels, and directives therefore cannot create the common visually-touching but
electrically-disconnected off-grid failure. The plan reports how many coordinates were adjusted;
KDS retains the authored coordinates as its sole persistent representation.

Repeated physical pins with the same number receive connectivity at every resolved location. A
missing unit or pin aborts before mutation. Native `kicad-cli sch export netlist` validation proves
the final files load and that the expected component references and net nodes are present. Cached
library symbols are reconciled by library ID inside `lib_symbols`; unmanaged cache entries are
preserved, and a same-ID unmanaged collision is rejected rather than overwritten.

Schematic electrical drawing primitives are top-level, stable-ID KDS forms with explicit sheet
ownership. A wire or bus is one native segment, and a bus entry uses the same endpoint spelling;
this keeps geometry easy for an LLM to inspect while avoiding signed-size conventions in authored
source:

```scheme
(wire sensor-leg
  (sheet analog)
  (from 40mm 40mm)
  (to 55mm 40mm)
  (stroke default default))

(bus sample-bus
  (sheet analog)
  (from 40mm 55mm)
  (to 80mm 55mm)
  (stroke 0.5mm dash))

(bus_entry sample-entry
  (sheet analog)
  (from 50mm 53.73mm)
  (to 51.27mm 55mm)
  (stroke default solid))

(junction sensor-branch
  (sheet analog)
  (at 55mm 40mm)
  (diameter 0.8mm)
  (color #11223380))

(label sensor-readable-name
  (sheet analog)
  (scope local)
  (net SENSOR_OUT)
  (at 55mm 40mm)
  (rotation 0deg)
  (shape none)
  (size 1.27mm 1.27mm)
  (thickness auto)
  (justify left bottom)
  (bold false)
  (italic false))

(rule_area analog-domain
  (sheet analog)
  (polygon
    (point 65mm 35mm) (point 90mm 35mm)
    (point 90mm 60mm) (point 65mm 60mm))
  (stroke 0.2mm dash_dot #11223380)
  (fill hatch #44556699)
  (exclude_from_sim false)
  (exclude_from_bom false)
  (exclude_from_board false)
  (dnp false))

(directive sensor-policy
  (sheet analog)
  (target net SENSOR_OUT)
  (at 55mm 40mm)
  (rotation 0deg)
  (shape round)
  (length 2.54mm)
  (property "Review Note" "route as controlled impedance"
    (at 56mm 38mm)
    (rotation 0deg)
    (size 1.27mm 1.27mm)
    (thickness auto)
    (justify left bottom)
    (bold false)
    (italic false)
    (visible true)))

(directive analog-domain-policy
  (sheet analog)
  (target rule_area analog-domain)
  (at 65mm 45mm)
  (rotation 0deg)
  (shape rectangle)
  (length 2.54mm)
  (property "Component Class" "ANALOG"
    (at 67mm 45mm)
    (rotation 0deg)
    (size 1.27mm 1.27mm)
    (thickness auto)
    (justify left center)
    (bold false)
    (italic true)
    (visible true)))

(text "AI-readable design note\nwith explicit rendering intent"
  (id analog-review-note)
  (sheet analog)
  (at 65mm 65mm)
  (rotation 15.5deg)
  (exclude_from_sim false)
  (size 1.2mm 1.5mm)
  (font stroke)
  (line_spacing 1.25)
  (thickness 0.2mm)
  (color #11223380)
  (justify right top)
  (mirror true)
  (bold true)
  (italic true)
  (hyperlink "https://example.com/design-review"))

(bus_alias SENSOR_SIGNALS
  (sheet analog)
  (members SENSOR_OUT SENSOR_ENABLE))
```

Stroke width is either `default` (native zero-width/net-class policy) or a bounded physical width;
the style is `default`, `solid`, `dash`, `dot`, `dash_dot`, or `dash_dot_dot`. Bus entries must be
diagonal and no more than 10 mm on either axis. Junction diameter is `auto` or a bounded physical
diameter, and color is `default` or `#RRGGBB[AA]`. Drawing IDs are unique across these forms and
compile to stable UUIDv8 identities. Zero-length segments, missing or unknown sheets, duplicate
fields, duplicate IDs, invalid styles, and out-of-range geometry fail compilation before mutation.
The lossless reconciler updates or removes only previously managed drawing UUIDs, while KiCad's
native schematic loader validates the complete result.

A label always references an existing KDS net with `(net NAME)`; its displayed native text is
derived from that canonical name instead of being authored a second time. Scope is `local` or
`global`. Local labels require `shape none`; global labels require `input`, `output`,
`bidirectional`, `tri_state`, or `passive`. Position, orthogonal rotation, rectangular font size,
`auto` or physical thickness, horizontal/vertical justification, bold, and italic state are all
explicit. Hierarchical labels are not duplicated as free-standing forms: they remain derived from
the child sheet's canonical `pin` declaration. Unknown nets and invalid scope/shape combinations
abort compilation.

A directive is the single KDS representation for a KiCad directive flag. Its target is either a
declared net or a declared rule area; the native marker's empty text is never copied into KDS.
Stable ID, sheet, anchor, orthogonal rotation, flag shape (`dot`, `round`, `diamond`, or
`rectangle`), and pin length are explicit. Each directive has 1 through 64 uniquely
named properties with a bounded string value and complete position, rotation, font size,
thickness, justification, bold, italic, and visibility state. The planner emits KiCad 10's current
`netclass_flag` spelling, not the accepted legacy `directive_label` alias. The same UUID ownership,
idempotence, removal, atomic journal, rollback, and native-loader validation used by other direct
schematic objects apply.

A rule area declares one 3-through-1024-point simple polygon. Repeated points, zero-area outlines,
self-intersections, and out-of-range coordinates fail compilation. Stroke always declares width,
native line style, and `default` or `#RRGGBB[AA]` color. Fill always declares a mode and color:
`none`, `outline`, and `background` require `default`; `color`, `hatch`, `reverse_hatch`, and
`cross_hatch` require `#RRGGBB[AA]`. Simulation, BOM, board-transfer, and DNP policy are explicit
booleans. A directive targeting a rule area must be on the same sheet and its anchor must lie
exactly on a polygon edge; an interior, exterior, or cross-sheet marker is rejected before apply.
The native rule-area UUID is nested in KiCad's `polyline`, so the reconciler deliberately validates
that location while still replacing or removing the complete `rule_area` atomically. Net targets
and rule-area targets now pass the complete production qualification, so
`schematic.directive_labels` is reported as `qualified`.

Top-level schematic `text` uses the same content-first, explicit-ID convention as board text, with
sheet ownership added. Content is a bounded UTF-8 string and may contain newlines and KiCad text
variables. Position, any normalized angle from 0 degrees through less than 360 degrees,
simulation inclusion, rectangular size, `stroke` or named font, line spacing, `auto` or physical
thickness, `default` or RGBA color, horizontal/vertical justification, mirror, bold, italic, and
`none` or a single-line hyperlink are all required. The compiler rejects duplicate or ignored
fields rather than inventing rendering defaults. Text lowers to KiCad 10's native `text` item with
a stable UUID and participates in the same idempotent reconcile, removal, native-loader gate, and
rollback journal.

Top-level schematic text boxes extend that same text contract with an explicit rectangular
container:

```scheme
(text_box "AI-readable constraint summary\nwith bounded context"
  (id analog-constraint-summary)
  (sheet analog)
  (at 80mm 60mm)
  (rotation 22.5deg)
  (box_size 30mm 12mm)
  (margins 0.5mm 0.75mm 1mm 1.25mm)
  (exclude_from_sim false)
  (stroke 0.3mm dash_dot #10203080)
  (fill cross_hatch #40506099)
  (text_size 1.2mm 1.5mm)
  (font "DejaVu Sans")
  (line_spacing 1.25)
  (thickness 0.2mm)
  (color #708090cc)
  (justify left top)
  (mirror false)
  (bold true)
  (italic false)
  (hyperlink "https://example.com/constraint-summary"))
```

`box_size` is independent of `text_size`, and `margins` always names left, top, right, then bottom.
Stroke width is `none` for KiCad's preserved hidden-border state, `default` for native zero-width
policy, or a physical width; line style and border color remain explicit in every case. Fill covers
all seven native modes: `none`, `outline`, and `background` pair with `default`, while `color`,
`hatch`, `reverse_hatch`, and `cross_hatch` require RGBA. The compiler bounds the box extent before
planning, and the stable UUID participates in the same guarded reconciliation and rollback path as
free text.

Schematic drawing geometry uses the native object name as its one canonical KDS representation:

```scheme
(polyline signal-flow (sheet analog)
  (points (point 20mm 90mm) (point 30mm 85mm) (point 40mm 90mm))
  (stroke 0.2mm dash #10203080)
  (fill none default))

(rectangle controller-boundary (sheet analog)
  (from 50mm 82mm) (to 75mm 97mm) (radius 2mm)
  (stroke default solid default)
  (fill color #40506099))

(circle inspection-window (sheet analog)
  (center 90mm 90mm) (radius 8mm)
  (stroke 0.2mm dot #708090cc)
  (fill hatch #10203080))

(arc current-path (sheet analog)
  (start 105mm 90mm) (mid 115mm 80mm) (end 125mm 90mm)
  (stroke 0.3mm dash_dot default)
  (fill background default))

(bezier response-curve (sheet analog)
  (points (point 135mm 90mm) (point 145mm 80mm)
    (point 155mm 100mm) (point 165mm 90mm))
  (stroke 0.25mm dash_dot_dot #a0b0c0dd)
  (fill reverse_hatch #11223344))
```

Polyline paths contain 2 through 1024 bounded points with no zero-length consecutive segment.
Rectangles use normalized `from` and `to` corners and an explicit zero-or-positive corner radius
that cannot exceed half the smaller dimension. Circles must remain entirely in the schematic
coordinate range. Arcs use KiCad's exact start/mid/end geometry and reject repeated or collinear
points. A cubic Bézier has exactly four ordered points: start, control 1, control 2, and end. Every
shape carries the same explicit hidden/default/physical stroke, line style, border color, fill mode,
fill color, sheet reference, and stable identity. They lower to KiCad 10's current native tokens and
pass the same reconcile, idempotence, removal, rollback, and native-loader gates. The broader
`schematic.text_graphics` facet also carries self-contained native images:

```scheme
(image system-overview
  (sheet analog)
  (at 180mm 90mm)
  (scale 1.25)
  (media_type image/png)
  (sha256 431ced6916a2a21a156e38701afe55bbd7f88969fbbfc56d7fe099d47f265460)
  (description "AI-readable block diagram of the controller signal flow")
  (data_base64 "..."))
```

An image always includes a non-empty semantic description so an AI can reason about its intent
without interpreting opaque pixels. The one embedded payload keeps the sidecar portable like a
native KiCad schematic. PNG, JPEG, GIF, BMP, and WebP signatures are recognized; MIME type and the
lowercase SHA-256 digest are checked against strict base64 decoding before planning. Decoded data
is bounded to 8 MiB, PNG dimensions are derived and bounded, and scale is explicit. The planner
chunks the verified bytes into KiCad 10's native `image/data` representation under a stable UUID;
the native image loader is part of the live qualification gate.

Schematic tables use one AI-native grid representation:

```scheme
(table pin-summary
  (sheet analog)
  (at 180mm 170mm)
  (rotation 90deg)
  (columns 18mm 27mm)
  (rows 7mm 7mm)
  (border
    (external true)
    (header true)
    (stroke 0.3mm dash #10203080))
  (separators
    (rows true)
    (columns false)
    (stroke default solid default))
  (cells
    (cell 1 1 "AI pin summary"
      (margins 0.5mm 0.6mm 0.7mm 0.8mm)
      (exclude_from_sim true)
      (fill color #40506099)
      (text_size 1.2mm 1.5mm)
      (font "DejaVu Sans")
      (line_spacing 1.25)
      (thickness 0.2mm)
      (color #708090cc)
      (justify left top)
      (mirror true)
      (bold true)
      (italic true)
      (hyperlink "https://example.com/pin-summary"))
    (cell 1 2 ""
      (margins 0mm 0mm 0mm 0mm) (exclude_from_sim false)
      (fill none default) (text_size 1.27mm 1.27mm) (font stroke)
      (line_spacing 1) (thickness auto) (color default)
      (justify center center) (mirror false) (bold false) (italic false)
      (hyperlink none))
    (cell 2 1 "VCC"
      (margins 0.4mm 0.4mm 0.4mm 0.4mm) (exclude_from_sim false)
      (fill background default) (text_size 1mm 1mm) (font stroke)
      (line_spacing 1) (thickness auto) (color default)
      (justify left center) (mirror false) (bold true) (italic false)
      (hyperlink none))
    (cell 2 2 "3V3 supply"
      (margins 0.4mm 0.4mm 0.4mm 0.4mm) (exclude_from_sim false)
      (fill none default) (text_size 1mm 1mm) (font stroke)
      (line_spacing 1) (thickness auto) (color default)
      (justify left center) (mirror false) (bold false) (italic false)
      (hyperlink none)))
  (merges (merge 1 1 1 2)))
```

Rows and columns are ordered dimensions; cells use clear 1-based row/column addresses and every
grid address is declared exactly once. Each cell preserves all native content, margins, simulation
inclusion, fill, font, size, line spacing, thickness, color, justification, mirroring, bold/italic,
and hyperlink state. A `merge` is one inclusive top-left/bottom-right rectangle. Merge rectangles
cannot overlap, and covered cells keep their formatting but must have empty content because the
content belongs to the merge's top-left cell. This is the only authored merge representation: the
planner deterministically derives KiCad's positive anchor span, zero spans for covered cells, and
the position and extent of every cell.

Tables contain 1 through 256 rows and columns and no more than 65,536 cells; individual and total
dimensions, anchor, and rotated extent are bounded to the schematic coordinate range. Rotation is
the native horizontal or vertical table orientation (`0deg` or `90deg`). Border and separator
visibility remain independent of their complete native stroke style and color. Table and cell
UUIDs are stable derivatives of the table ID and cell address. Reconciliation owns the table as one
atomic native item, while the real KiCad 10 schematic loader validates nested cells and merge spans.
The qualified `schematic.text_graphics` facet therefore covers free text, text boxes, every native
free graphic, embedded images, tables, and table cells.

A bus alias is `(bus_alias NAME (sheet ID) (members NET ...))`. Every member references a declared
KDS net, member names are unique, and each alias contains 1 through 256 members. Alias names are
unique within a sheet. KiCad's native bus-alias item has no UUID, so KiChad records a stable sidecar
identity but reconciles the native item by sheet and alias name. A same-name alias not proven to be
previously managed is never claimed; apply fails before mutation. Repeated apply is byte-idempotent,
and removing the KDS declaration removes only the alias named in prior managed state.

Schematic groups use one typed, AI-readable containment representation:

```scheme
(group signal-core
  (sheet root)
  (name "AI signal core")
  (locked true)
  (members
    (member component R1 1)
    (member drawing root-wire)
    (member net_label SENSOR_OUT R1 1 1 1)))

(group review-bundle
  (sheet root)
  (name "AI review bundle")
  (locked false)
  (members
    (member sheet analog)
    (member group signal-core)))
```

Every member names the semantic KDS object instead of exposing an opaque native UUID. The complete
member vocabulary is `drawing ID`, `component REF UNIT`, `sheet CHILD_SHEET_ID`,
`hierarchical_label SHEET_ID PIN_NAME`, `net_label NET REF UNIT PIN OCCURRENCE`,
`no_connect REF UNIT PIN OCCURRENCE`, and `group GROUP_ID`. Occurrences are 1-based and select one
native item when a library unit contains repeated pin numbers. The planner resolves every typed
member to its stable UUID only after exact symbol pins and hierarchy items exist.

A group and all of its direct members must live on the same native schematic screen. A child sheet
symbol therefore belongs to a group on its parent sheet, while that child sheet's hierarchical
labels belong to groups inside the child. An item can have only one direct parent group; nested
groups must be acyclic, and empty groups are rejected because KiCad does not serialize them. Group
ID, visible name, lock state, and member UUID set all participate in guarded reconciliation,
idempotence, rollback, and the real KiCad 10 loader gate. KiChad also closes KiCad 10's native
schematic lock persistence gap so `(locked yes)` is read, retained by the group object, and written
again. Design-block library links are intentionally not overloaded onto this form: they remain the
separate `project.design_blocks` capability until their authoring and update workflow is complete.

### Library dependencies and project tables

KDS uses one library declaration for installed and project-local dependencies:

```scheme
(library symbol Device (table global))
(library footprint Resistor_SMD (table global))
(library symbol ProductSymbols (table project)
  (uri "${KIPRJMOD}/libraries/product.kicad_sym"))
(library footprint ProductFootprints (table project)
  (uri "${KIPRJMOD}/libraries/Product.pretty")
  (managed true))
(library model ProductModels (table project)
  (uri "${KIPRJMOD}/libraries/models"))
```

`global` declares a required installed nickname and forbids a URI. `project` requires one bounded,
project-confined `${KIPRJMOD}/` URI; symbol libraries end in `.kicad_sym` and footprint libraries
end in `.pretty`. Traversal, absolute paths, quoted path injection, duplicate fields, duplicate
kind/nickname pairs, and more than 512 dependencies are compile errors. Model dependencies use the
same declaration but do not generate a table because KiCad has no model-library table.

When any libraries are declared, KDS owns the complete project symbol and footprint tables. Apply
deterministically lowers the project entries to native `sym-lib-table` and `fp-lib-table` files,
validates both with KiCad 10's library-table parser, and installs them atomically beside the project
file. Global dependencies are intentionally not copied into project tables. Exact prior presence
and bytes for both tables are stored in the apply journal and restored in reverse order after a
lost acknowledgement or any later pre-commit failure. Existing table symlinks, malformed generated
rows, type/version mismatches, and tables larger than 1 MiB or 256 rows are rejected before
mutation. These native files are compiler artifacts; the authored `.kicad_kds` declarations remain
the only external design representation.

### KDS-owned symbol libraries

KDS can explicitly own and compile a complete project symbol library. Ownership is opt-in because
replacing an editor-managed library implicitly would be destructive:

```scheme
(library symbol ProductSymbols (table project)
  (uri "${KIPRJMOD}/libraries/ProductSymbols.kicad_sym")
  (managed true))

(symbol ProductSymbols:SENSOR
  (reference U (at 0mm -2.54mm) (visible true) (justify center bottom))
  (value SENSOR (at 0mm 2.54mm) (visible true) (justify center top))
  (datasheet "https://example.com/SENSOR.pdf")
  (description "Two-pin sensor")
  (keywords "sensor precision")
  (body_styles "Normal" "Alternate")
  (units_locked true)
  (footprint_filter "Package_SO:*")
  (footprint_filter "Package_DFN_QFN:*")
  (property "Manufacturer" "Example Semiconductor"
    (at 0mm 3.81mm) (visible true) (show_name true) (autoplace false)
    (private false) (rotation 0deg) (size 1.27mm 1.27mm)
    (font stroke) (line_spacing 1) (thickness auto) (color default)
    (justify center top) (bold false) (italic false) (hyperlink none))
  (pin_names_offset 0.254mm)
  (unit common (body_style 1)
    (rectangle body (from -2.54mm -1.27mm) (to 2.54mm 1.27mm)
      (radius 0.2mm) (stroke 0.254mm default) (fill background))
    (circle target (center 0mm 0mm) (radius 0.5mm))
    (arc accent (start -1mm 0mm) (mid 0mm 1mm) (end 1mm 0mm))
    (bezier curve (start -1mm -0.5mm) (control1 -0.5mm -1mm)
      (control2 0.5mm -1mm) (end 1mm -0.5mm))
    (polyline marker (point -0.5mm 0mm) (point 0mm 0.5mm) (point 0.5mm 0mm))
    (text label "SENSOR" (at 0mm 2mm) (size 1mm 1mm) (justify center bottom))
    (text_box note "Calibrated" (at -2mm -3mm) (box_size 4mm 1.5mm)
      (margins 0.1mm 0.1mm 0.1mm 0.1mm)))
  (unit 1 (body_style 1) (display_name "Sensor channel")
    (pin 1 (name IN) (electrical input) (shape line)
      (at -5.08mm 0mm) (orientation right) (length 2.54mm)
      (alternate GPIO bidirectional line))
    (pin 2 (name OUT) (electrical output) (shape inverted)
      (at 5.08mm 0mm) (orientation left) (length 2.54mm))))
```

`symbol` is semantic KDS, not embedded native s-expression text. A `common` unit maps graphics to
native unit zero; numbered units range from 1 through 256. A root symbol can declare the exact
body-style inventory once as `(body_styles demorgan)` or as 1 through 64 unique display names.
Each unit's optional `body_style` selects that 1-based inventory. A numbered unit may carry one
bounded `display_name`; declarations for the same unit across body styles must agree because KiCad
stores one display name per logical unit. `(units_locked true)` preserves non-interchangeable unit
semantics. Repeated `footprint_filter` forms carry unique whitespace-free library patterns and
lower to KiCad's native filter inventory in declaration order. Derived aliases inherit all four
facets and cannot contradict their root. The qualified lowering covers mandatory metadata, custom properties, inclusion flags,
pin-name/number visibility, and all five KiCad vector primitives: rectangles (including rounded
corners), circles, three-point arcs, four-point cubic Beziers, and arbitrary polylines. Every
graphic has a stable logical ID and may declare `(private true)`. Strokes support `default`,
`solid`, `dash`, `dot`, `dash_dot`, and `dash_dot_dot`; fills support `none`, `outline`,
`background`, `color`, `hatch`, `reverse_hatch`, and `cross_hatch`. A stroke or fill can carry an
explicit `(color RED GREEN BLUE ALPHA)` with integer RGB channels and alpha from zero through one.
`text` and `text_box` expose private state, exact 0.1-degree rotation, width/height, stroke or named
fonts, automatic or explicit thickness, bold/italic state, line spacing, RGBA color, horizontal and
vertical justification, and absolute-URI or internal-sheet hyperlinks. Text boxes additionally
expose positive box size, left/top/right/bottom margins, and the complete stroke/fill model.
Hidden symbol text is intentionally not accepted because KiCad converts it into a field rather
than preserving it as text. The same lowering covers KiCad's complete electrical pin-type and
pin-shape enumerations.
Pins may carry any number of uniquely named `(alternate NAME ELECTRICAL_TYPE SHAPE)` functions;
these lower to KiCad's native alternate pin assignments without changing the physical pin number.

Mandatory `reference`, `value`, `footprint`, `datasheet`, `description`, and `keywords` forms and
every custom `property` use the same inline field-layout vocabulary shown above. Layout includes an
absolute bounded position, exact 0.1-degree rotation, visibility, name display, editor-autoplace
permission, privacy, width/height, stroke or named font, line spacing, automatic or explicit
thickness, default or RGBA color, horizontal/vertical justification, bold/italic state, and an
absolute/internal hyperlink. Omitted settings use deterministic KiCad editor defaults; they are
never inferred from graphic bounds. Derived aliases can apply the same layout to their overridden
fields. Hidden fields remain native fields—unlike hidden symbol text, KiCad preserves them exactly.

A root symbol may declare `(power global)` or `(power local)`; `normal` is the default. Power
symbols require a `#` reference and at least one `power_in` pin, preventing a visually plausible
symbol from silently losing KiCad's native power-net semantics. A derived alias uses one
same-library parent and declares no units of its own:

```scheme
(symbol ProductSymbols:VCC_BASE
  (reference "#PWR") (value VCC) (power global)
  (unit 1
    (pin 1 (name VCC) (electrical power_in) (at 0mm 0mm) (length 0mm))))
(symbol ProductSymbols:VCC
  (extends VCC_BASE) (value VCC) (description "Positive supply"))
```

Derived symbols can override field values and custom properties while inheriting units, graphics,
pins, and power behavior. Missing parents, cross-library names, self-reference, and recursive
inheritance chains are compile/generation errors before filesystem mutation.

Jumpered packages can declare `(duplicate_pin_numbers_are_jumpers true)` and explicit
`(jumper_group PIN_NUMBER PIN_NUMBER...)` sets. Every group needs at least two existing unique pin
numbers, and a pin cannot occur in multiple groups. Both forms lower to KiCad's native connection
graph semantics, so ERC and exported netlists see the same internal connections as the sidecar.
Coordinates are explicitly dimensioned, bounded to ±2 m, and lowered
to exact decimal millimetres without floating-point formatting drift. Pin orientations use the
cardinal words `right`, `down`, `left`, and `up`.

All top-level symbols targeting a managed library are sorted into one deterministic current-format
`.kicad_sym` compiler artifact (`version 20251024`). A managed library must contain at least one
symbol, cannot target a global nickname, and cannot be mixed silently with unmanaged library
content. Before apply, the generated library is used directly for exact symbol/unit/pin resolution,
so schematic planning and the installed artifact consume identical bytes. Apply journals exact
prior presence and bytes, atomically installs the library after its project table, asks KiCad
10.0.4's native symbol loader to parse and resave an isolated copy, and only then installs generated
schematics. Native rejection or any later pre-commit failure restores the prior symbol library,
tables, schematics, and settings in reverse order. Parent directories must already exist and file
symlinks are rejected. Embedded-font ownership and unmanaged-library publishing remain explicit
partial coverage rather than being accepted and ignored.

### KDS-owned footprint libraries

KDS can likewise own a complete project `.pretty` library. The semantic source contains no native
s-expression escape hatch; current KiCad 10 `.kicad_mod` files are deterministic compiler
artifacts:

```scheme
(library footprint ProductFootprints (table project)
  (uri "${KIPRJMOD}/libraries/ProductFootprints.pretty")
  (managed true))

(footprint ProductFootprints:SENSOR_2P
  (reference U)
  (value SENSOR_2P)
  (datasheet "https://example.com/SENSOR_2P.pdf")
  (description "Two-pad production sensor")
  (keywords "sensor smd")
  (attributes
    (smd true)
    (exclude_from_position false)
    (exclude_from_bom false)
    (allow_missing_courtyard true))
  (net_tie_group 1 2)
  (pad input
    (number 1)
    (type smd)
    (shape roundrect)
    (at -0.8mm 0mm)
    (rotation 0deg)
    (size 0.8mm 0.8mm)
    (layers F.Cu F.Mask F.Paste)
    (roundrect_radius 0.2mm)
    (teardrop
      (enabled true)
      (target_length 0.5 1mm)
      (target_width 1 2mm)
      (edges curved)
      (track_width_limit 0.9)
      (allow_two_segments true)
      (prefer_zone_connections true))
    (pin_function INPUT)
    (pin_type signal)
    (solder_mask_margin 0.02mm)
    (solder_paste_margin_ratio -0.1)
    (zone_connection thermal)
    (thermal_spoke_width 0.2mm)
    (thermal_spoke_angle 45deg)
    (thermal_gap 0.15mm))
  (pad output
    (number 2)
    (type smd)
    (shape rect)
    (at 0.8mm 0mm)
    (rotation 90deg)
    (size 0.8mm 1mm)
    (layers F.Cu F.Mask F.Paste))
  (model "${KIPRJMOD}/models/SENSOR_2P.step"
    (visible true)
    (opacity 0.75)
    (offset 0mm 0mm 0.1mm)
    (scale 1 1 1)
    (rotation 0deg 0deg 90deg)))
```

Each `pad` begins with a stable logical ID, distinct from its electrical `number`. Supported types
are `smd`, `connect`, `thru_hole`, and `np_thru_hole`; standard shapes are `circle`, `rect`, `oval`,
`trapezoid`, and `roundrect`. `chamfered_rect` adds one explicit physical radius, a chamfer ratio,
and one through four named corners. `custom` has one semantic representation made from named copper
primitives rather than embedded native syntax:

```scheme
(pad thermal_tab
  (number 3) (type smd) (shape custom) (at 0mm 0mm)
  (rotation 0deg) (size 0.4mm 0.4mm) (layers F.Cu F.Mask F.Paste)
  (custom
    (anchor rect)
    (clearance convex_hull)
    (line stem (start -0.6mm 0mm) (end 0.6mm 0mm) (width 0.2mm))
    (circle center (center 0mm 0mm) (radius 0.35mm) (fill true))
    (polygon tip (point 0.3mm -0.2mm) (point 0.8mm 0mm)
      (point 0.3mm 0.2mm) (fill true))))

(pad pin_one
  (number 1) (type smd) (shape chamfered_rect) (at 0mm 0mm)
  (size 1mm 0.8mm) (layers F.Cu F.Mask F.Paste)
  (roundrect_radius 0.1mm) (chamfer_ratio 0.25)
  (chamfer top_left bottom_right))
```

Custom primitives are `line`, `rectangle`, `arc`, `circle`, `polygon`, and `bezier`. They use the
same non-ambiguous geometry as footprint artwork: circles are center plus radius, arcs use three
points, polygons have implicit closure, and curves have two named control points. Primitive width
defaults to zero; rectangles, circles, and polygons alone accept `fill true|false`. IDs are stable
KDS handles even though KiCad does not assign UUIDs to a custom pad's internal primitives.

Layer tokens are explicit: `F.Cu`, `B.Cu`, their mask/paste/adhesive
layers, plus `all_copper` and `all_mask` for the native wildcard sets. Through-hole pads require
`(drill round DIAMETER)` or `(drill oval WIDTH HEIGHT)` and `all_copper`; surface pads cannot carry
a drill. A numberless `smd` pad may instead be a front- or back-side technical aperture containing
only that side's paste, mask, or adhesive layers; this represents the independent stencil windows
used by WSON/QFN exposed pads without inventing electrical copper. Aperture pads spanning both sides
or carrying an electrical number are rejected. Electrical-only metadata (pin semantics, teardrops,
zone/thermal/clearance policy, or copper padstacks) is also rejected on apertures, and mask/paste
margin overrides require the corresponding technical layer. The generated numberless native pads
are loaded by KiCad's own footprint parser before installation. NPTH pads cannot have an electrical
number. Physical radius, trapezoid, layer-removal, and drill/type constraints are rejected by the
compiler before any file mutation.

Pads may also declare `shape_offset`, `trapezoid_delta`, a typed native `property`, `die_length`,
pin function/type metadata, and local solder-mask, solder-paste, clearance, zone-connection, thermal
spoke, and thermal-gap overrides. Every override uses `inherit` when the footprint should retain the
board or zone policy; implicit sentinel values are not exposed to the model. Jumper and net-tie
groups name existing electrical pad numbers and are checked for duplicate membership.

Electrically connectable pads use the same complete `teardrop` form as vias. The preferred length
and width ratios, absolute maxima, straight or curved edges, track-width filter, two-segment policy,
and zone-connection preference are all explicit. This single KDS representation lowers both to
current KiCad 10 footprint syntax and to the native typed Pad IPC message, preserving the same state
through managed-library import, live editing, and save/reload.

Plated through-hole pads can replace the single-shape stack with one explicit semantic `padstack`.
The `front_inner_back` mode requires both `inner` and `B.Cu`; the front shape is the pad's ordinary
top-level geometry. The `custom` mode accepts sparse `In1.Cu` through `In30.Cu` and `B.Cu`
overrides, with unspecified copper layers inheriting the front geometry exactly as KiCad does:

```scheme
(pad plated_pin
  (number 1) (type thru_hole) (shape circle) (at 0mm 0mm)
  (size 2mm 2mm) (layers all_copper all_mask) (drill round 1mm)
  (backdrills
    (top (diameter 1.3mm) (stop_layer In1.Cu))
    (bottom (diameter 1.3mm) (stop_layer In2.Cu)))
  (padstack front_inner_back
    (layer inner
      (shape custom) (size 0.6mm 0.6mm)
      (custom
        (anchor circle)
        (line spoke (start -0.6mm 0mm) (end 0.6mm 0mm) (width 0.25mm))
        (circle hub (center 0mm 0mm) (radius 0.35mm) (fill true)))
      (thermal_spoke_width 0.2mm) (thermal_gap 0.15mm))
    (layer B.Cu
      (shape chamfered_rect) (size 1.8mm 1.8mm)
      (roundrect_radius 0.2mm) (chamfer_ratio 0.2)
      (chamfer top_left top_right) (zone_connection solid)))
  (hole_treatment
    (tenting (front open) (back tented))
    (post_machining front counterbore
      (diameter 1.6mm) (depth 0.3mm))))
```

Every layer entry requires its own shape and size, then may independently set offset, trapezoid
delta, rounding/chamfer, custom copper primitives, clearance, zone connection, thermal spoke
width/angle, and thermal gap. Per-layer custom geometry uses the same named `custom` primitives as
a top-level custom pad. It omits `clearance` because KiCad stores that zone-clearance mode once for
the whole pad rather than per copper layer.
`backdrills` names manufacturing sides rather than KiCad's secondary/tertiary storage fields. Each
operation requires a diameter larger than the primary drill and an inner-copper stop layer. When
both sides are present, they must leave at least one non-overlapping plated span; a custom footprint
stack also requires both stop layers to exist in that exact stack.
`hole_treatment` uses manufacturing words rather than inverted booleans: `open` means a solder-mask
opening, `tented` means solder mask covers that side, and `inherit` leaves board policy in control.
Counterbores require diameter and depth; countersinks require diameter and included angle. The
finished diameter must exceed the primary drill.

Footprint artwork is semantic rather than a raw s-expression escape hatch. Each primitive has one
shape-specific representation and a stable logical ID:

```scheme
(line ID (start X Y) (end X Y) GRAPHIC_STYLE)
(rectangle ID (start X Y) (end X Y) (radius DISTANCE) GRAPHIC_STYLE)
(arc ID (start X Y) (mid X Y) (end X Y) GRAPHIC_STYLE)
(circle ID (center X Y) (radius DISTANCE) GRAPHIC_STYLE)
(polygon ID (point X Y) (point X Y) (point X Y) ... GRAPHIC_STYLE)
(bezier ID (start X Y) (control1 X Y) (control2 X Y) (end X Y) GRAPHIC_STYLE)

; GRAPHIC_STYLE is written directly in each primitive:
(stroke WIDTH solid|dash|dash_dot|dash_dot_dot|dot)
(layers LAYER...)
(fill none|solid|hatch|reverse_hatch|cross_hatch) ; rectangle/circle/polygon only
(locked true|false)
(solder_mask_margin inherit|DISTANCE)
```

The six forms lower to `fp_line`, `fp_rect`, `fp_arc`, `fp_circle`, `fp_poly`, and
`fp_curve`. A circle is authored as center plus radius, never as an arbitrary edge point. Polygon
closure is implicit, so the first point must not be repeated. The compiler rejects coincident line
ends, zero-area rectangles and polygons, collinear arcs, invalid corner radii, duplicate polygon
closure, incompatible fills, and mask-margin layer sets. A local mask margin is only valid on the
exact `F.Cu F.Mask` or `B.Cu B.Mask` layer pair. Stable UUIDv8 identities derive from the footprint
and primitive IDs, so coordinate edits do not replace object identity.

User text and text boxes use the same explicit font and justification vocabulary:

```scheme
(text body_reference "${REFERENCE}"
  (at 0mm 0mm) (rotation 0deg) (layer F.Fab)
  (font (face default) (size 0.5mm 0.5mm) (line_spacing 1)
    (thickness 0.08mm) (bold false) (italic false))
  (justify center center false) ; horizontal, vertical, mirrored
  (locked false) (keep_upright true) (knockout false))

(text_box pin_one "PIN 1"
  (box -1.9mm -1.4mm -0.6mm -0.8mm)
  ; A polygon outline is the one alternative:
  ; (polygon (point X Y) (point X Y) (point X Y) ...)
  (rotation 0deg) (layer F.Fab)
  (margins 0.05mm 0.05mm 0.05mm 0.05mm)
  (font (face default) (size 0.3mm 0.3mm) (thickness 0.05mm))
  (justify center center false)
  (stroke 0.05mm solid)
  (border true) (knockout false) (locked false)
  (hyperlink "https://example.com/pin-one"))
```

Font size is always explicit and ordered `HEIGHT WIDTH`; `thickness auto` retains KiCad's derived
stroke thickness. Justification is always `HORIZONTAL VERTICAL MIRRORED_BOOL`, avoiding positional
defaults that are hard for a model to infer. Text boxes accept exactly one non-zero rectangular or
non-degenerate polygon outline and explicit four-sided margins. Generated text, text-box, and
graphic objects all receive stable identities and are accepted and resaved by the KiCad 10 native
footprint loader during apply. An optional single-line hyperlink is preserved inside native text
effects and is bounded to 2048 UTF-8 bytes.

Footprint-wide design rules use one explicit effective-policy form. `inherit` is the only spelling
for a native inherited value, so zero remains a real authored value rather than a hidden sentinel:

```scheme
(rules
  (clearance 0.1mm)
  (solder_mask_margin -0.02mm)
  (solder_paste_margin inherit)
  (solder_paste_margin_ratio -0.1)
  (zone_connection thermal))
```

When present, all five fields are required. The form lowers directly to the footprint's native
clearance, solder-mask, solder-paste, and zone-connection overrides. A footprint that does not
declare `rules` inherits every board policy.

Footprint-local custom copper stacks and private layers are also semantic declarations:

```scheme
(stackup custom (layers F.Cu In1.Cu In2.Cu B.Cu))
(private_layers User.1 F.Fab)
```

The custom stack must contain 2 through 32 even copper layers ordered `F.Cu`, contiguous `InN.Cu`,
then `B.Cu`. Absence of `stackup` is the one representation for KiCad's ordinary expand-inner
mode. Named zone layers, private copper layers, and custom padstack overrides are checked against
the authored stack. A `front_inner_back` padstack is rejected with a custom footprint stack because
its generic `inner` layer would be ambiguous; named custom padstack layers remain exact.

Reusable footprint metadata has one stable-ID property representation. The logical ID controls
identity and grouping while `name` is the human-visible field name, so renaming or repositioning a
property does not silently replace its native object:

```scheme
(property manufacturer_part_number
  (name "Manufacturer Part Number")
  (value "MODULE-BASE")
  (at 0mm 1.5mm)
  (rotation 0deg)
  (layer F.Fab)
  (visible false)
  (keep_upright true)
  (knockout false)
  (font (face default) (size 0.8mm 0.8mm)
    (line_spacing 1) (thickness auto) (bold false) (italic false))
  (justify center center false))
```

Every top-level display field is explicit. Property names are unique case-insensitively and cannot
collide with `Reference`, `Value`, `Datasheet`, or `Description`. Groups may reference a property by
logical ID, and variants may override only an existing non-Reference mandatory or custom property.
The compiler emits current KiCad property syntax with a deterministic UUID and full font,
justification, visibility, orientation, keep-upright, and knockout semantics.

Footprint component classes are an explicit set of native class names:

```scheme
(component_classes "Precision Analog" "Thermal Critical")
```

The form accepts 1 through 256 unique bounded names and emits them in canonical lexical order.
Names remain the actual Board Setup/DRC class identifiers; KDS does not invent a second aliasing
layer. KiChad also preserves unresolved class names while a `.kicad_mod` is loaded and resaved
without board context, preventing whole-library validation from silently removing this metadata.

Footprint zones use one semantic form with an explicit `purpose`; there is no second keepout syntax
and no native s-expression escape hatch:

```scheme
(zone exposed_pad_copper
  (purpose copper)
  (name "Exposed-pad local copper")
  (layers F.Cu)
  (outline (polygon
    (point -2mm -1.5mm) (point 2mm -1.5mm)
    (point 2mm 1.5mm) (point -2mm 1.5mm)
    (hole (point -0.4mm -0.4mm) (point -0.4mm 0.4mm)
      (point 0.4mm 0.4mm) (point 0.4mm -0.4mm))))
  (clearance 0.15mm)
  (min_thickness 0.2mm)
  (connection thermal
    (thermal_gap 0.2mm) (thermal_spoke_width 0.25mm))
  (islands remove_below (minimum_area 0.2mm2))
  (fill hatched
    (thickness 0.25mm) (gap 0.2mm) (orientation 45deg)
    (edge_smoothing fillet (amount 0.25))
    (hole_min_area_ratio 0.1) (border hatch))
  (priority 2)
  (border diagonal_edge (pitch 0.5mm))
  (corner_smoothing fillet (radius 0.15mm))
  (locked false))

(zone antenna_keepout
  (purpose keepout)
  (layers all_copper F.Mask)
  (outline (polygon
    (point -4mm -3mm) (point 4mm -3mm)
    (point 4mm 3mm) (point -4mm 3mm)))
  (prohibit
    (copper true) (vias true) (tracks true)
    (pads false) (footprints false))
  (border solid)
  (locked true))
```

Copper zones require explicit clearance, minimum thickness, pad connection, island, and fill
policies. Keepouts require all five direct prohibitions and at least one must be true. Both forms
validate bounded, non-zero, non-self-intersecting polygon geometry; holes must be strictly inside
and mutually disjoint. KiCad's footprint file format represents one outer contour plus holes per
zone, so a disjoint area is another stable `zone` declaration instead of an ambiguous multi-outline
encoding. `all_copper` is the semantic wildcard available to keepouts; copper zones name their
actual copper layers. Fill, thermal, border, priority, corner, lock, and rule-area placement policy
lower to current KiCad 10 zone fields.

Groups reference typed logical IDs, never native UUIDs. A member belongs to exactly one group,
nested groups may be declared in any order, and cycles are rejected:

```scheme
(group electrical_core
  (name "Electrical core") (locked false)
  (member pad ground)
  (member graphic shield_edge)
  (member text assembly_note)
  (member property manufacturer_part_number))

(group complete_module
  (name "Complete module") (locked true)
  (member group electrical_core)
  (member zone antenna_keepout))
```

Assembly variants likewise have one explicit effective-state representation. All three production
flags are required so a model never has to infer an inherited DNP/BOM/position state. Variant
fields may override bounded footprint metadata, but `Reference` is immutable because changing it
would change schematic-to-board identity:

```scheme
(variant production
  (dnp false)
  (exclude_from_bom false)
  (exclude_from_position false)
  (field "Value" "SHIELDED_MODULE-PROD")
  (field "Manufacturer Part Number" "MODULE-001"))
```

Model assignments accept project-confined `${KIPRJMOD}` files or installed-stock
`${KICAD10_3DMODEL_DIR}` files and require STEP, STP, or WRL paths with bounded offset, scale,
rotation, visibility, and opacity. Preview and apply resolve the active KiCad versioned stock-model
root, canonicalize every referenced file, reject missing/empty/oversized/symlinked or escaping
assets, and report checked project/stock counts before launching an editor or mutating a document.
Referencing a project model does not acquire or embed its bytes, so sourcing and package-data
workflows must still provide that project asset separately; stock models remain portable with the
KiCad 10 library installation.

All footprints are sorted and emitted as KiCad 10 format `20260206` files with deterministic UUIDv8
field, property, pad, graphic, text, text-box, zone, and group identities. Apply uses the generated sources directly for compiler-created board
instances, snapshots the exact prior whole library, atomically swaps a staging directory into place,
and asks `kicad-cli fp upgrade --force` to parse and resave every file in an isolated directory.
Unexpected files, subdirectories, symlinks, native rejection, or any later pre-commit failure abort
the operation and restore the prior library, project tables, schematics, and settings in reverse
order. Each placed footprint carries the SHA-256 of its exact resolved native source in managed
state. A library-ID change or same-ID source revision uses the PCB editor's native atomic footprint
exchange inside the existing commit. The exchange collision-checks any identity migration, safely
reconciles child and pad UUIDs, preserves group/routing relationships, reapplies authoritative KDS
instance metadata and pad nets, and verifies identity, placement, orientation, lock/DNP state,
symbol link, metadata, pad/net inventory, and 3D-model inventory from KiCad's response before the
commit can close. A replacement or readback rejection drops the commit; recovery from an uncertain
journal clears only the source revision so the next apply must re-verify the footprint rather than
trusting stale geometry.
Footprint authoring remains intentionally partial: embedded assets and complete
package-data-to-KLC geometry generation remain explicit capability gaps. Plugging,
filling, capping, and covering are KiCad via—not footprint-pad—file semantics and are tracked under
the board-via capability.

### Stackup form

KDS authors one explicit top-to-bottom physical stack. Copper-layer count and total board thickness
are derived from it; they are deliberately not accepted as separate fields that could disagree with
the authored layers.

```scheme
(stackup
  (finish "ENIG")
  (impedance_controlled true|false)
  (edge_connector none|yes|bevelled)
  (edge_plating true|false)
  (layers
    ; optional top technical layers, only in this order
    (silkscreen F.SilkS (material "Epoxy ink") (color "White"))
    (solderpaste F.Paste)
    (soldermask F.Mask (thickness 10um) (material "LPI")
      (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))

    (copper F.Cu (thickness 35um))
    (dielectric core (thickness 0.486mm) (material "FR408HR")
      (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
    (copper In1.Cu (thickness 18um))
    (dielectric prepreg (thickness 0.486mm) (material "FR408HR 2116")
      (epsilon_r 3.66) (loss_tangent 0.0092) (locked false))
    (copper In2.Cu (thickness 18um))
    (dielectric core (thickness 0.517mm) (material "FR408HR")
      (epsilon_r 3.68) (loss_tangent 0.0092) (locked true))
    (copper B.Cu (thickness 35um))

    ; optional bottom technical layers, only in this order
    (soldermask B.Mask (thickness 10um) (material "LPI")
      (epsilon_r 3.5) (loss_tangent 0.025) (color "Green"))
    (solderpaste B.Paste)
    (silkscreen B.SilkS (material "Epoxy ink") (color "White"))))
```

All four global fabrication policies and the `layers` declaration are required. A stack has 2
through 32 even copper layers ordered `F.Cu`, sequential `In1.Cu` through `In30.Cu`, then `B.Cu`,
with exactly one `core` or `prepreg` dielectric between adjacent copper layers. Physical board
layers cannot repeat. Copper and dielectric thickness, dielectric material, relative permittivity,
loss tangent, and thickness lock are explicit. Solder-mask physical properties and silkscreen
material/color are explicit when those optional layers are present. Total thickness is the sum of
copper, dielectric, and solder-mask thicknesses and must not exceed 20 mm. The former
`copper_layers`/`thickness` shorthand is invalid KDS: there is one stackup representation.

### Global board rules form

KDS authors KiCad's complete global Board Setup constraint set once, as a single top-level
declaration:

```scheme
(rules
  (minimum_clearance 0.2mm)
  (minimum_connection_width 0.15mm)
  (minimum_track_width 0.18mm)
  (minimum_via_annular_width 0.1mm)
  (minimum_via_diameter 0.6mm)
  (minimum_through_hole_diameter 0.3mm)
  (minimum_microvia_diameter 0.3mm)
  (minimum_microvia_drill 0.1mm)
  (minimum_hole_to_hole 0.25mm)
  (minimum_copper_to_hole_clearance 0.25mm)
  (minimum_silkscreen_clearance -0.01mm)
  (minimum_groove_width 0.3mm)
  (minimum_resolved_spokes 3)
  (minimum_silkscreen_text_height 0.8mm)
  (minimum_silkscreen_text_thickness 0.08mm)
  (minimum_copper_to_edge_clearance legacy)
  (use_height_for_length_calculations true)
  (maximum_error 0.005mm)
  (allow_fillets_outside_zone_outline false)
  (drc_severities
    (default error)
    (check lib_footprint_mismatch warning))
  (erc_severities
    (check single_global_label error)
    (check footprint_filter error)))
```

Every field is required when `rules` is present, so a design never inherits an accidental local
constraint. `minimum_copper_to_edge_clearance` accepts a non-negative physical distance or the
semantic value `legacy`; scripts never encode KiCad's internal negative sentinel. Via and microvia
diameters must be large enough for the declared drill plus twice the minimum annular width. Native
KiCad ranges are enforced before planning. `maximum_error` is the curve-to-segment approximation
tolerance, while `allow_fillets_outside_zone_outline` controls KiCad's external zone smoothing.
The former generic top-level `(rule NAME ...)` form is invalid; global constraints have this one
representation. Conditional custom rules are a separate KiCad concept and use the non-overlapping
`custom_rules` form below.

`drc_severities` is optional, but when present it owns the complete native DRC policy: exactly one
`default` is required and named `check` entries override it. KiChad validates every check key
against the checks registered by the pinned KiCad build, replaces the native severity map through
typed PCB IPC, reads it back exactly, and journals it for rollback. `erc_severities` is also
optional and contains named `check` overrides; omitted ERC checks retain KiCad's pinned native
defaults. Both forms accept only `error`, `warning`, or `ignore`, reject duplicate or malformed
keys, persist in the project file, and are rerun by the release gate. Remaining ignored checks are
reported explicitly and block fabrication unless the release request carries a visible waiver.

### Net classes form

KDS has one complete net-class table. The `Default` class is declared first; every other class
follows in descending precedence, so declaration order is the priority and there is no second
numeric-priority spelling. All classes precede assignments.

```scheme
(net_classes
  (class Default
    (clearance 0.2mm) (track_width 0.2mm)
    (via_diameter 0.6mm) (via_drill 0.3mm)
    (microvia_diameter 0.3mm) (microvia_drill 0.1mm)
    (diff_pair_width 0.18mm) (diff_pair_gap 0.2mm) (diff_pair_via_gap 0.22mm)
    (tuning_profile none) (pcb_color default)
    (wire_width 0.15mm) (bus_width 0.3mm)
    (schematic_color default) (line_style solid))
  (class USB_HS
    (clearance inherit) (track_width 0.15mm)
    (via_diameter inherit) (via_drill inherit)
    (microvia_diameter inherit) (microvia_drill inherit)
    (diff_pair_width 0.15mm) (diff_pair_gap 0.18mm) (diff_pair_via_gap inherit)
    (tuning_profile usb_hs) (pcb_color "#1A66CCDD")
    (wire_width inherit) (bus_width inherit)
    (schematic_color "#22AA44") (line_style dash_dot))
  (assign (pattern "USB_[PN]") (classes USB_HS))
  (assign (pattern "/usb/D[PN]") (classes USB_HS)))
```

All fifteen class fields are required so omissions cannot silently acquire machine-local defaults.
`Default` requires concrete distances, `default` colors, `none` or a named tuning profile, and an
explicit line style. Other classes use either a concrete value or `inherit`; colors use `inherit`
or `#RRGGBB`/`#RRGGBBAA`; line styles are `solid`, `dash`, `dot`, `dash_dot`, or `dash_dot_dot`.
Wire and bus widths must be exactly representable in KiCad schematic units (100 nm). Via and
microvia diameters cannot be smaller than their effective drills after inheritance.

Each `assign` contains one bounded KiCad net-name pattern and one or more declared non-Default
classes. Pattern/class pairs are unique. Bus ranges may contain at most 256 members per range and
the complete table may expand to at most 4096 native assignments. Class names are unique without
regard to case; exact spelling is used by assignments. The compiler limits the table to 256 classes
and 1024 authored pattern/class pairs.

Apply lowers this form to one typed `NetClassSettings` message. KiChad validates the complete table,
priorities, native ranges, colors, padstacks, assignments, and physical cross-constraints before
replacing any project setting. It journals the previous table and restores it on lost
acknowledgements or any later pre-commit failure. The older partial merge API is not used by KDS;
there is one source representation and one atomic replacement operation.

### Custom rules form

KDS authors the complete conditional custom-rule set once. There is no second KDS spelling and no
authored `.kicad_dru` companion. The compiler deterministically lowers this form to KiCad's native
rule document as an internal board artifact:

```scheme
(custom_rules
  (rule usb_diff_pair
    (condition "A.hasNetclass('USB_HS')")
    (layer outer)
    (severity error)
    (constraint diff_pair_gap (min 0.15mm) (opt 0.2mm) (max 0.25mm))
    (constraint skew (domain diff_pairs) (max 5ps)))
  (rule assembly_policy
    (condition always)
    (layer all)
    (severity warning)
    (constraint disallow (items track through_via pad footprint))
    (constraint assertion
      (test "A.Type != 'Footprint' || A.Orientation != 13deg"))))
```

Every rule uses exactly this clause order: name, `condition`, `layer`, `severity`, then one or more
constraints. Conditions are either `always` or one quoted native KiCad rule expression. Layers are
`all`, `outer`, `inner`, or one bounded native layer/pattern such as `F.Cu` or `In*.Cu`.
Severities are `ignore`, `warning`, `error`, or `exclusion`. Rule names are unique, a rule contains
each constraint type at most once, and source containing an unresolved `${...}` expression is
rejected. The native KiCad parser and expression compiler validate layer patterns and conditions
again before the active board can change.

KDS exposes all 35 KiCad 10 custom constraint types through structured values:

- Bare flags: `(constraint via_dangling)` and `(constraint bridged_mask)`.
- Assertion: `(constraint assertion (test "EXPRESSION"))`.
- Zone connection: `(constraint zone_connection (style solid|thermal_reliefs|none))`.
- Prohibition: `(constraint disallow (items ...))`, with unique items in canonical order:
  `track`, `via`, `through_via`, `blind_via`, `buried_via`, `micro_via`, `pad`, `zone`, `text`,
  `graphic`, `hole`, `footprint`. The aggregate `via` cannot be combined with individual via types.
- Counts and ratios: `(constraint min_resolved_spokes (count 0..4))` and
  `(constraint solder_paste_rel_margin (ratio -10..10))`; paste ratios have exact 0.001
  resolution, so `-0.1` means a ten-percent reduction.
- Distance ranges: `annular_width`, `clearance`, `creepage`, `connection_width`,
  `courtyard_clearance`, `diff_pair_gap`, `diff_pair_uncoupled`, `edge_clearance`,
  `hole_clearance`, `hole_size`, `hole_to_hole`, `physical_clearance`,
  `physical_hole_clearance`, `silk_clearance`, `solder_mask_expansion`,
  `solder_mask_sliver`, `solder_paste_abs_margin`, `text_height`, `text_thickness`,
  `thermal_relief_gap`, `thermal_spoke_width`, `track_width`, `track_segment_length`, and
  `via_diameter`.
- Typed ranges: `track_angle` uses `deg`; `via_count` uses non-negative integers; `length` and
  `skew` use either distances or `fs`/`ps` time values. A `skew` also requires
  `(domain nets|diff_pairs)`.

Range values use the supported `(min VALUE)`, `(opt VALUE)`, and `(max VALUE)` fields for that
native constraint, occurring once in that order, with `min <= opt <= max`. Required native fields
are enforced—for example, `clearance` requires `min`, `diff_pair_uncoupled` requires `max`, and
paste/mask optimal-margin constraints require `opt`. A single range cannot mix distance and time.
Values are bounded to KiCad's exact numeric domains before planning.

The compiler emits one deterministic native document, normalizing physical distances losslessly to
decimal millimetres, time to femtoseconds, and angles to the native unitless degree field. The PCB
Editor endpoint limits the artifact to 1 MiB, 512 rules, 64 constraints per rule, and 4096 total
constraints. It validates UTF-8, parses and compiles the complete document, installs it atomically,
reloads the live DRC engine, and reads it back byte-for-byte. Failure restores both the previous
file and previous engine rules. KDS journals that exact prior state and restores it on a lost
acknowledgement or any later pre-commit failure.

### Footprint placement and field presentation

`place` owns the native position, rotation, side, and lock state of a schematic-linked footprint.
It can also own any selected presentation properties of that footprint's Reference and Value
fields. The text itself continues to come from the component reference/value, so presentation
changes cannot accidentally rename a component or alter its BOM identity.

```scheme
(place U2
  (at 24mm 18mm)
  (rotation 90deg)
  (side front)
  (locked false)
  (reference
    (visible true)
    (layer F.SilkS)
    (at 21mm 18mm)
    (size 1mm 1mm)
    (stroke 0.15mm)
    (angle 90deg)
    (justify center bottom)
    (font stroke)
    (bold false)
    (italic false)
    (underlined false)
    (mirrored false)
    (keep_upright true))
  (value
    (visible false)
    (layer F.Fab)))
```

Every field setting is optional, but an included `reference` or `value` block must contain at
least one setting. `at` is an absolute board coordinate, which stays unambiguous across footprint
rotation and front/back flipping. Layers are limited to the front/back silkscreen and fabrication
layers appropriate for assembly presentation. Sizes range from 1 um through 250 mm; a supplied
stroke is positive and no greater than one quarter of the supplied smaller text dimension.

Apply uses nested typed field masks, preserving every property the KDS did not name. The PCB
Editor merges the request with the live footprint, applies it transactionally, and returns the
native footprint for exact readback before KiChad reports success. This makes legible silkscreen
and assembly drawings a reusable KDS property rather than a board-specific post-processing step.

### Physical layout contract

KDS carries physical acceptance intent in the same `board` form as the exact manufactured
geometry. It is not a second layout file and it does not replace exact placement or copper. The
contract gives an AI stable, semantic relationships to author and gives KiChad deterministic
measurements with actionable failures instead of asking a model to judge pixels alone.

```scheme
(layout
  (board
    (maximum_width 100mm)
    (maximum_height 80mm))
  (placement
    (near C7 U2 (maximum 5mm))
    (align J1 J2 (axis y) (tolerance 0.5mm))
    (edge J1 right (maximum 3mm))
    (group motor_power
      (members J1 C7 U2)
      (maximum_span 25mm)))
  (routing
    (defaults
      (geometry octilinear)
      (maximum_vias 4))
    (net VMOT
      (maximum_vias 0)
      (maximum_length 45mm))
    (net RF_OUT
      (geometry any)
      (maximum_vias 2)
      (maximum_length 30mm))
    (bundle motor_phases
      (nets M1A M1B M2A M2B)
      (maximum_skew 5mm))))
```

`board` is required and declares positive maximum X/Y extents for the exact outline. Extents are
measured over line, rounded-rectangle, polygon, circle, exact three-point arc, and cubic-Bezier
outline geometry. A board can therefore be rectangular, curved, cut out, or an arbitrary closed
profile without acquiring a different representation.

Placement relationships use component references and their native footprint anchors:

- `near` bounds Euclidean anchor distance.
- `align` bounds X- or Y-axis anchor displacement.
- `edge` bounds anchor distance from a named outline extent.
- `group` bounds the greatest pairwise anchor distance among two or more functionally related
  components.

These forms are deliberately electrical-domain neutral. The same vocabulary describes a compact
sensor, a rack backplane, an RF module, a motor controller, or a flex interconnect; no component or
board-family templates are hidden in the compiler.

`routing defaults` is required. `geometry` is `orthogonal`, `octilinear` (axis-aligned or exact
45-degree straight segments), or `any` for designs that intentionally use arcs or arbitrary
angles. `maximum_vias` applies independently to every routed net. A `net` form overrides any
subset of the defaults and may add an exact maximum total copper length. A `bundle` bounds the
difference between the longest and shortest named net. Arc length is measured from its exact
three-point circle rather than its chord.

`verify` with operation `layout` compiles the exact `.kicad_kds`, measures the contract, and
returns complete counts plus bounded pageable issues. It never changes the board. Production
intent requires both `(check layout)` and a clean layout result; fabrication planning also reports
layout failures immediately, so ERC/DRC-clean geometry cannot by itself claim production
readiness.

The optional `synthesize` policy fills physical details that were not authored exactly:

```scheme
(synthesize
  (placement
    (grid 1mm) (clearance 0.5mm) (edge_clearance 1mm)
    (rotations 0deg 90deg 180deg 270deg))
  (routing
    (grid 0.25mm) (clearance 0.2mm) (width 0.25mm) (layer F.Cu)))
```

Placement preserves every explicit `place`, sorts remaining components by connectivity and stable
reference, reads pad bounds plus native front/back courtyard geometry from the exact inventoried
footprint, and performs a deterministic bounded grid search inside one rectangular board outline. Routing
preserves every explicitly routed net, maps schematic pin numbers to native footprint pads, and
uses deterministic Manhattan search around footprint courtyards, track keepouts, previously
synthesized copper, and existing traces/arcs/vias on the selected layer. It emits ordinary exact
`place` and `route` IR consumed by the same transactional backend. If no legal result exists it
fails with corrective steering; it never emits crossing fallback copper or silently moves authored
geometry.

This is a bounded, deterministic baseline for common rectangular boards rather than a replacement for expert
RF, impedance, length-tuned, differential, or multilayer routing. Exact `place`, `route`, `via`,
zone, net-class, custom-rule, and layout-contract forms remain available in the same KDS file for
those constraints and for curved, cutout, or multi-island outlines. Native DRC/layout verification
remains mandatory after synthesis.

### Board outline geometry

The manufactured contour is a collection of stable-ID `Edge.Cuts` graphics using the same six
semantic primitives as footprint artwork. There is no raw KiCad geometry form and no separate
cutout syntax: closed inner circles or polygons are cutouts, while additional closed outer contours
are independent board islands.

```scheme
(outline
  (line lower (start 0mm 20mm) (end 30mm 20mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none))
  (arc right_end (start 30mm 20mm) (mid 35mm 10mm) (end 30mm 0mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none))
  (bezier upper (start 30mm 0mm) (control1 20mm -1mm)
    (control2 10mm 1mm) (end 0mm 0mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none))
  (line left (start 0mm 0mm) (end 0mm 20mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none))
  (circle mounting_cutout (center 5mm 5mm) (radius 1.5mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none))
  (polygon connector_cutout
    (point 12mm 7mm) (point 18mm 7mm) (point 18mm 10mm) (point 12mm 10mm)
    (stroke 0.05mm solid) (layers Edge.Cuts) (fill none)))
```

`rectangle` additionally accepts a corner `radius`; polygons close implicitly. Every primitive is
unfilled, solid-stroked, and explicitly on `Edge.Cuts`, and lowers to KiCad 10's typed
`BoardGraphicShape` API. The final DRC/fabrication gate remains responsible for whole-contour
topology such as endpoint closure, self-intersections, and illegal nesting.

The same primitives may appear directly inside `board` for general artwork on any supported board
layer. One layer creates one native shape. A paired `F.Cu F.Mask` or `B.Cu B.Mask` declaration owns
the copper shape plus its native solder-mask replication, with an optional local margin. Copper
graphics may name a real schematic net:

```scheme
(circle shield_logo
  (center 15mm 12mm) (radius 1mm)
  (stroke 0.15mm solid)
  (layers F.Cu F.Mask)
  (fill none)
  (net GND)
  (solder_mask_margin 0.05mm)
  (locked true))
```

Board graphics support physical, technical, sequential inner-copper, and `User.1` through
`User.45` layers. Stroke style is `solid`, `dash`, `dash_dot`, `dash_dot_dot`, or `dot`; typed board
fill is `none` or `solid`. Net ownership and mask expansion are carried by KiChad's extended
`BoardGraphicShape` IPC message and round-trip through native `PCB_SHAPE` state.

### Board routing and via protection

Routes are explicit line or three-point arc geometry with stable IDs, a real schematic net, copper
layer, width, and lock state. Vias likewise have one physical form for through, blind, buried, and
microvia spans. Their optional `protection` block records manufacturer-facing intent directly and
lowers into KiCad 10's typed `PadStack` IPC message:

```scheme
(via USB_D_P
  (id usb-dp-via)
  (at 18mm 12mm)
  (diameter 0.8mm)
  (drill 0.4mm)
  (layers F.Cu B.Cu)
  (type through)
  (unconnected_layers keep_start_end)
  (force_flash F.Cu)
  (teardrop
    (enabled true)
    (target_length 0.5 1mm)
    (target_width 1 2mm)
    (edges curved)
    (track_width_limit 0.9)
    (allow_two_segments true)
    (prefer_zone_connections true))
  (locked false)
  (protection
    (tenting (front open) (back tented))
    (covering (front covered) (back inherit))
    (plugging (front plugged) (back unplugged))
    (filling filled)
    (capping uncapped)
    (post_machining front counterbore
      (diameter 0.6mm) (depth 0.15mm))))
```

Every sided treatment names both `front` and `back`; use `inherit` to retain the board rule. Tenting
uses `open|tented`, covering uses `covered|uncovered`, and plugging uses `plugged|unplugged` so an
LLM never has to infer an inverted boolean. Filling and capping use equally direct state words.
Treatments on a physical side are rejected unless the via span reaches that outer copper layer.
Counterbores require diameter and depth; countersinks require diameter and included angle. Their
diameter must exceed the primary drill.

`unconnected_layers` controls annular copper without an inverted pair of booleans: `keep` preserves
all rings, `remove` strips every unconnected ring, `keep_start_end` strips only intermediate
unconnected rings, and `start_end_only` keeps copper exclusively on the drilled span's endpoints.
The selected policy is carried directly in KiCad's typed padstack and survives readback.
When removal is active, `force_flash` names the copper layers that must retain an explicit
zone-connected annular ring; all other layers follow the selected removal policy. KiChad extends
the typed Via IPC message with this native state so live create/update/readback and `.kicad_pcb`
save/reload have the same semantics.

`teardrop` carries complete per-via geometry and connection policy. `target_length` and
`target_width` pair the preferred pad/via-size ratio with an absolute maximum, while
`track_width_limit` filters tracks by their width relative to the landing geometry. Edge shape,
two-segment use, and whether a direct zone connection is preferred are explicit semantic values.
The same KDS form is shared with footprint pads, so an AI learns one teardrop vocabulary instead of
separate editor and file-format controls.

An advanced via replaces—not supplements—the simple circular `diameter` with exactly one semantic
`padstack`. `front_inner_back` requires `F.Cu`, `inner`, and `B.Cu`. `custom` requires every named
copper layer in the via's drilled span, so no unspecified layer silently inherits geometry:

```scheme
(via DDR_DQ0
  (id ddr-dq0-via)
  (at 24mm 16mm)
  (drill 0.3mm)
  (layers F.Cu B.Cu)
  (type through)
  (padstack custom
    (layer F.Cu (shape custom) (size 0.5mm 0.5mm)
      (custom
        (anchor circle)
        (line spoke (start -0.3mm 0mm) (end 0.3mm 0mm) (width 0.12mm))
        (circle hub (center 0mm 0mm) (radius 0.25mm) (fill true))))
    (layer In1.Cu (shape oval) (size 0.8mm 0.6mm))
    (layer In2.Cu (shape rect) (size 0.7mm 0.7mm) (offset 0.05mm 0mm))
    (layer B.Cu
      (shape chamfered_rect) (size 0.8mm 0.7mm)
      (roundrect_radius 0.05mm) (chamfer_ratio 0.2)
      (chamfer top_left bottom_right)))
  (backdrills
    (top (diameter 0.5mm) (stop_layer In1.Cu))
    (bottom (diameter 0.55mm) (stop_layer In2.Cu))))
```

Via-layer `custom` geometry is exactly the same named line, rectangle, arc, circle, polygon, and
Bezier representation used by footprint pads. KiChad deterministically lowers those primitives to
typed `BoardGraphicShape` entries inside the official `PadStackLayer` IPC message; no native syntax
or opaque geometry payload enters KDS.

Per-layer shapes are `circle`, `rect`, `oval`, `trapezoid`, `roundrect`, and `chamfered_rect`, with
the same one-representation geometry used by footprint pads. Every copper shape must exceed the
primary drill. `backdrills` names physical `top` and `bottom` operations instead of exposing
KiCad's internal secondary/tertiary numbering; each operation requires a larger diameter and one
inner stop layer. Two-sided backdrills must leave a non-empty plated layer span. Per-layer custom
graphic primitives, unconnected-layer removal, forced-flash policy, and per-item teardrop geometry
and policy all lower through the typed KiCad 10 API.

### Copper zone form

The canonical KDS version 1 copper-zone form is:

```scheme
(zone NET
  (id LOGICAL_ID)
  (name "OPTIONAL DISPLAY NAME")
  (layers F.Cu In1.Cu B.Cu)
  (outline
    (polygon
      (point X Y) (point X Y) (point X Y)
      (hole (point X Y) (point X Y) (point X Y)))
    (polygon ...))
  (clearance DISTANCE)
  (min_thickness DISTANCE)
  (connection none|solid)
  ; or: (connection thermal|pth_thermal
  ;       (thermal_gap DISTANCE) (thermal_spoke_width DISTANCE))
  (islands remove_all|keep_all)
  ; or: (islands remove_below (minimum_area AREA))
  (fill solid)
  ; or: (fill hatched
  ;       (thickness DISTANCE) (gap DISTANCE) (orientation ANGLE)
  ;       (smoothing 0..1) (hole_min_area_ratio 0..1) (border minimum|hatch))
  ; optional for hatched fill:
  ; (hatch_offsets (layer F.Cu X Y) (layer B.Cu X Y))
  (priority UNSIGNED_INTEGER)
  (border solid|invisible)
  ; or: (border diagonal_full|diagonal_edge (pitch DISTANCE))
  (locked true|false))
```

`id`, `layers`, `outline`, `clearance`, `min_thickness`, `connection`, `islands`, and `fill`
are required. `name` defaults to the logical ID, `priority` to zero, `border` to solid, and
`locked` to false. Polygon closure is implicit, so the first point must not be repeated. Every
outer line and hole needs at least three unique non-collinear points and may not self-intersect.
Holes must be strictly inside and mutually disjoint; multiple outer polygons must also be disjoint.
A zone is bounded to 32 polygons, 64 holes per polygon, and 8192 total points; layers must be
distinct and present in the declared stackup. KDS validates these constraints before any editor
connection or mutation. Hatched zones may additionally define one bounded offset per zone layer;
solid zones reject hatch offsets instead of retaining ignored intent.

### Keepout form

The canonical KDS version 1 keepout form is:

```scheme
(keepout
  (id LOGICAL_ID)
  (name "OPTIONAL DISPLAY NAME")
  (layers F.Cu In1.Cu B.Cu)
  (outline
    (polygon
      (point X Y) (point X Y) (point X Y)
      (hole (point X Y) (point X Y) (point X Y))))
  (prohibit
    (copper true|false)
    (vias true|false)
    (tracks true|false)
    (pads true|false)
    (footprints true|false))
  (border solid|invisible)
  ; or: (border diagonal_full|diagonal_edge (pitch DISTANCE))
  (locked true|false))
```

`id`, `layers`, `outline`, and `prohibit` are required. Every prohibited category is explicit and
at least one must be true, so an ineffective rule area is rejected. `name` defaults to the logical
ID, `border` to solid, and `locked` to false. Keepouts use distinct copper layers present in the
declared stackup and the same bounded, exact polygon topology contract as copper zones. They lower
to KiCad's native `ZT_RULE_AREA` with placement-area behavior explicitly disabled; they are not
copper zones and are never awaited as filled objects.

### Board text form

The canonical KDS version 1 board text form is:

```scheme
(text "CONTENT"
  (id LOGICAL_ID)
  (layer F.SilkS)
  (at X Y)
  (size WIDTH HEIGHT)
  (stroke WIDTH)
  (angle ANGLE)
  (justify left|center|right top|center|bottom)
  (font stroke|"INSTALLED FONT NAME")
  (line_spacing RATIO)
  (bold true|false)
  (italic true|false)
  (underlined true|false)
  (mirrored true|false)
  (keep_upright true|false)
  (hyperlink "OPTIONAL URI")
  (knockout true|false)
  (locked true|false))
```

`id`, `layer`, `at`, `size`, and `stroke` are required. Text is bounded to 64 KiB and uses LF line
endings; whether it is multiline is derived from the content instead of represented twice. Size is
bounded to KiCad's native 1 um through 250 mm range, stroke is capped at one quarter of the smaller
text dimension, and coordinates must fit KiCad's internal board coordinate range. Angle defaults to
0 degrees, justification to centered, font to KiCad's portable stroke font, line spacing to 1, and
all style, knockout, and lock booleans to false. Named fonts are preserved exactly and must be
installed anywhere the design is rendered. All normal KiCad copper, technical, fabrication, and
user board layers are accepted; copper layers are checked against the declared stackup and the
target board must have the authored layer enabled.

### Board text-box form

Board and footprint text boxes share one KDS representation. On a board it lowers to the native
typed `BoardTextBox` object, including the outline that the stock API previously discarded:

```scheme
(text_box assembly_note "Keep this area clear\nSee assembly guide"
  (box 4mm 4mm 28mm 12mm)
  ; Or use one non-degenerate outline:
  ; (polygon (point 4mm 4mm) (point 28mm 4mm)
  ;          (point 26mm 12mm) (point 4mm 12mm))
  (rotation 0deg)
  (layer F.SilkS)
  (margins 0.5mm 0.5mm 0.5mm 0.5mm)
  (font (face default) (size 1.2mm 1.2mm)
        (line_spacing 1) (thickness 0.18mm)
        (bold false) (italic false))
  (justify left top false)
  (stroke 0.15mm solid)
  (border true)
  (knockout false)
  (locked false)
  (hyperlink "https://example.com/assembly"))
```

The ID, content, one rectangle or polygon, rotation, layer, margins, font, justification, border
stroke, border state, knockout state, and lock state are explicit. Hyperlink is optional and
bounded to one 2048-byte line. The rectangle or polygon, margins, stroke style, and border policy
round-trip through the extended KiCad 10 protobuf instead of being reconstructed from a bounding
box. Board tables and table cells deliberately remain a separate KDS implementation.

### Board reference-image and barcode forms

KDS owns reference-image bytes directly. The image is portable with the sidecar, bounded to 8 MiB,
checked against its declared file signature, and bound by SHA-256 before KiChad will plan a board
mutation:

```scheme
(image assembly-reference
  (at 90mm 70mm)
  (origin_offset 0mm 0mm)
  (layer Dwgs.User)
  (scale 1)
  (locked true)
  (media_type image/png)
  (sha256 431ced6916a2a21a156e38701afe55bbd7f88969fbbfc56d7fe099d47f265460)
  (description "Digest-owned assembly reference image")
  (data_base64 "..."))
```

The position, transform-origin offset, scale, board layer, lock state, and exact decoded bytes lower
to KiCad's native `ReferenceImage`. `description` and `media_type` remain AI-readable ownership
metadata; the native object preserves the decoded image rather than an external path.

Native barcodes are semantic objects, not pre-rendered artwork:

```scheme
(barcode board-identity
  (text "KICHAD-STEPPER-1.0.0")
  (kind code_39|code_128|data_matrix|qr_code|micro_qr_code)
  (error_correction L|M|Q|H)
  (at 55mm 67mm)
  (rotation 0deg)
  (layer F.SilkS)
  (size 10mm 10mm)
  (show_text false)
  (text_height 1.2mm)
  (knockout false)
  (knockout_margin 0.5mm 0.5mm)
  (locked true))
```

All fields are explicit and bounded. Both forms receive stable project-derived UUIDs, participate in
the managed-state ownership manifest, use typed create/update/delete operations, and are read back
through the same PCB tool as every other managed board item.

### Board table form

Board tables reuse the same 1-based, row/column AI-native grid model as schematic tables, adding
the board layer and lock state while omitting schematic-only simulation and fill fields:

```scheme
(table pin-summary
  (at 30mm 40mm)
  (rotation 90deg)
  (layer F.Fab)
  (locked true)
  (columns 18mm 27mm)
  (rows 7mm 8mm)
  (border
    (external true) (header true)
    (stroke 0.3mm dash #10203080))
  (separators
    (rows true) (columns false)
    (stroke default solid default))
  (cells
    (cell 1 1 "AI pin summary"
      (margins 0.5mm 0.6mm 0.7mm 0.8mm)
      (text_size 1.2mm 1.5mm) (font "Noto Sans")
      (line_spacing 1.25) (thickness 0.2mm)
      (justify left top) (mirror true) (bold true) (italic true)
      (hyperlink "https://example.com/pins") (locked true))
    (cell 1 2 ""
      (margins 0mm 0mm 0mm 0mm)
      (text_size 1mm 1mm) (font stroke) (line_spacing 1) (thickness auto)
      (justify center center) (mirror false) (bold false) (italic false)
      (hyperlink none) (locked false))
    (cell 2 1 "VCC"
      (margins 0.4mm 0.4mm 0.4mm 0.4mm)
      (text_size 1mm 1mm) (font stroke) (line_spacing 1) (thickness auto)
      (justify left center) (mirror false) (bold true) (italic false)
      (hyperlink none) (locked false))
    (cell 2 2 "3V3 supply"
      (margins 0.4mm 0.4mm 0.4mm 0.4mm)
      (text_size 1mm 1mm) (font stroke) (line_spacing 1) (thickness auto)
      (justify left center) (mirror false) (bold false) (italic false)
      (hyperlink none) (locked false)))
  (merges (merge 1 1 1 2)))
```

Every grid address is declared exactly once. Merge rectangles are non-overlapping and inclusive;
only their top-left anchor may contain text, while the compiler derives KiCad's positive anchor
span and zero covered-cell spans. Rows, columns, cells, dimensions, margins, font state, hyperlink,
and cell lock state are all bounded. Border exterior/header lines and separator row/column lines
have independent visibility plus complete native width, style, and RGBA color. The table and every
owned cell receive deterministic UUIDs and lower as one atomic typed `BoardTable` transaction.

### Dimension form

KDS has one canonical dimension form whose style selects one of KiCad's five native dimension
geometries:

```scheme
(dimension aligned|orthogonal|radial|leader|center
  (id LOGICAL_ID)
  (layer Dwgs.User)
  ; aligned/orthogonal: (from X Y) (to X Y) (height SIGNED_DISTANCE)
  ;                     (extension_height DISTANCE), plus (axis x|y) for orthogonal
  ; radial:             (center X Y) (radius_point X Y) (leader_length DISTANCE)
  ; leader:             (from X Y) (to X Y)
  ;                     (border none|rectangle|circle|roundrect) (label "TEXT")
  ; center:             (center X Y) (to X Y)
  ; aligned, orthogonal, and radial measurement policy:
  (units mm|mil|in|automatic)
  (unit_format no_suffix|bare_suffix|paren_suffix)
  (precision fixed_0|fixed_1|fixed_2|fixed_3|fixed_4|fixed_5|
             scaled_in_2|scaled_in_3|scaled_in_4|scaled_in_5)
  (suppress_trailing_zeroes true|false)
  (prefix "TEXT") (suffix "TEXT") (override "TEXT")
  ; aligned and orthogonal layout policy:
  (arrow_direction inward|outward)
  (text_position outside|inline|manual)
  ; also accepted by radial dimensions:
  (keep_text_aligned true|false)
  ; non-center text appearance:
  (text_at X Y)
  (text_size WIDTH HEIGHT)
  (text_stroke WIDTH)
  (text_angle ANGLE)
  (text_justify left|center|right top|center|bottom)
  (font stroke|"INSTALLED FONT NAME")
  (bold true|false) (italic true|false)
  (underlined true|false) (mirrored true|false)
  ; common physical policy:
  (line_width DISTANCE)
  (arrow_length DISTANCE)
  ; aligned, orthogonal, and leader only:
  (extension_offset DISTANCE)
  (locked true|false))
```

`id`, `layer`, `line_width`, and `arrow_length` are required for every style. Geometry fields are
required exactly as shown and endpoints must differ. Measurement fields are required for aligned,
orthogonal, and radial dimensions; radial dimensions default to the conventional `R ` prefix.
`text_size` and `text_stroke` are required for every style except center marks. `text_at` is required
for radial and leader dimensions and whenever aligned or orthogonal text is manual; it is rejected
when automatic positioning would ignore it. An explicit text angle is likewise rejected while
`keep_text_aligned` is true. Leader labels compile to KiCad's native override text, while center
marks reject all text and measurement fields. Fields that do not apply to the selected style are
errors instead of silently retaining intent. The compiler lowers every style into the same native
KiCad `Dimension` protobuf type and uses the authored style as its single geometry oneof.

## Compiler pipeline

1. Parse bounded, lossless s-expressions.
2. Type-check every executable form, required field, identifier, and reference.
3. Resolve libraries, symbols, footprints, models, components, pins, and nets.
4. Produce a deterministic IR, source digest, diagnostics, and mutation plan.
5. Establish the whole-project pre-apply snapshot.
6. Compile native project library tables, exact cached symbols, placed units, connectivity, and
   hierarchy through lossless edits validated by KiCad 10.
7. Compile live board state through the official KiCad 10 protobuf IPC transaction API.
8. Record live-search sourcing evidence in the canonical KDS source forms.
9. Run ERC, DRC, connectivity, sourcing, and manufacturability checks.
10. Package and validate exact firmware plus the programming/power/bring-up handoff.
11. Generate and validate requested fabrication outputs.

Compilation and planning are read-only. Apply requires the exact previewed source SHA-256, the
pre-turn project snapshot, and the target board open in PCB Editor. Managed PCB ownership is
validated by recomputing every deterministic UUID from project, item kind, and KDS logical ID;
unmanaged collisions abort before a transaction begins. Existing managed items are updated,
missing ones are recreated, and only previously managed obsolete items are deleted in a single
KiCad transaction. A project-confined apply journal makes an interrupted operation safely
reconcilable on the next apply, while the whole turn remains revertible from local history.

The apply backend currently executes nested native schematic hierarchy, confined project-local
symbol resolution, multi-unit and virtual/power component placement, native inclusion flags,
global-net connectivity, explicit no-connect state, native local/global labels, directive flags,
schematic rule areas, free text, bus aliases, wires/junctions/buses/bus entries, complete native project
symbol/footprint tables, physical
board stackups, the complete global Board Setup
constraint and DRC severity set, named ERC severity overrides, complete net-class tables, all 35 conditional custom-rule types, rectangular
outlines, component placement, straight traces, arcs, vias, copper zones, keepout rule areas,
native board text, and all five native dimension styles. Stackup apply
uses KiCad's native protobuf stackup message and editor
endpoint; it validates the entire ordered structure before mutation, derives enabled physical
layers and board thickness, and preserves rather than deletes objects when technical layers are
disabled. Removing a non-empty copper layer is rejected. The pre-apply stack is retained in the
apply journal and restored if a later settings or transactional item mutation fails. Global rules
use a typed native protobuf endpoint, validate every field and physical cross-constraint before
mutation, and are journaled and restored together with the stackup on any pre-commit failure. A
named ERC severity map uses a typed project endpoint, is saved durably with exact readback, and is
journaled and restored before later project and board mutations. A
complete net-class table uses a typed native project endpoint, is persisted with the project, and
is journaled and restored after the global rules. Custom rules use a bounded native board endpoint,
are compiled by KiCad's real DRC parser and engine, and are journaled and restored after net classes
and before project library tables and transactional PCB items. Project library tables are parsed by
KiCad before atomic installation and journaled as exact prior bytes. KDS-owned symbol libraries
are then installed from deterministic current-format source, validated by KiCad's native symbol
loader, and journaled with exact rollback bytes before schematic reconciliation. A zone explicitly declares
its net, stable ID, one or more copper layers, bounded
polygon/hole geometry, clearance, minimum thickness, connection and thermal policy, island policy,
solid or hatched fill, priority, border display, and lock state. No manufacturing setting is
inherited silently. Zone creation and updates are committed through KiCad 10 IPC, after which
KiCad's official refill operation is polled until every desired zone reports filled. Refill failure
retains the recovery journal and aborts the apply result rather than claiming success.
Keepouts use a separate deterministic ownership type and exact `rule_area_settings` update mask, so
their unfilled state is never confused with a failed copper refill.

Placement resolves exactly one schematic component by reference. If its footprint is already on the
board and its reported library identity matches KDS, KDS updates position, rotation, front/back
side, lock state, Value, DNP, Datasheet, Description, and the exact component-property field set in
place. Footprint UUID, symbol path, pads, graphics, models, and child UUIDs remain unchanged. If the
KDS component changes to a different footprint library entry, apply replaces the complete live
footprint from the resolved source inside the same transaction, preserving desired placement,
presentation, symbol linkage, and pad nets; a rejected exchange drops the transaction and restores
the original complete footprint. An
unreported live library identity fails with explicit steering rather than pretending a metadata
update changed geometry. If the footprint is absent, a declared project-local `.pretty` library or an installed
KiCad stock global library may supply the exact `.kicad_mod`: KiChad parses it with KiCad's
native parser and atomically creates a footprint instance with the component reference, value,
DNP flag, deterministic root UUID and hierarchical symbol path, sheet metadata, placement, and pad
nets. The sidecar records ownership of compiler-created and explicitly replaced instances. A later
apply deletes such an instance by exact UUID when the placement is removed and recreates the same
UUID when restored. A matching user-owned footprint resolved by reference remains unowned; changing
its declared library identity is an explicit request to replace it and transfer ownership to KDS.
Project sources are
size-bounded, project-confined regular files; stock global sources must resolve inside KiCad's
installed stock-library root. Path-shaped entry names and undeclared or unavailable content are
rejected. The resolver is generic by declared library nickname and entry—there is no Arduino,
Pololu, or board-specific lookup table. User-global and Plugin and Content Manager library tables
are not silently searched; portable projects should declare project libraries when a part is not
in the installed stock set. Duplicate references,
board-only matches, malformed sources, missing connected pads, or unavailable sources abort the
transaction before commit.
Any other structurally retained form is refused before mutation until it has its own typed backend
and rollback coverage.

Run `tools/smoke-kichad-kds-apply.sh --allow-mutation` for the opt-in live proof. The harness creates
an isolated temporary project, starts its own build-tree PCB Editor, applies one lossless physical
stackup plus twelve authored PCB primitives and one compiler-owned footprint, and reapplies the
unchanged source to verify updates reuse the same deterministic identities. It reads the stackup
back from the live editor and verifies finish,
impedance, bevel, plating, all nine ordered physical layers, thicknesses, material, color, loss,
permittivity, and dielectric lock. It also reads back all global constraint fields, including the
semantic legacy edge-clearance mode, and verifies the second apply updates the same settings. It
verifies both generated project library tables byte-for-byte through repeated apply. It
reads back the complete net-class table and assignments, including inherited fields, native
schematic units, colors, line styles, via/microvia padstacks, and priorities; an invalid replacement
must leave that table unchanged. It also reads back the exact generated custom-rule document,
reapplies it without duplication, and proves malformed native input leaves the active document
unchanged. It then
places an existing schematic-linked footprint on the back side and proves its footprint/symbol/pad
identities and flipped child layers survive both applies. It also creates a second footprint from a
confined project-local library, verifies its deterministic root identity, exact hierarchical symbol
link and pad net, then removes and recreates it with the same UUID. The committed native fixtures
use the exact KiCad 10.0.4 board, schematic, symbol, and footprint format versions, which the smoke
test checks before editor launch. It proves the fifth managed object is a filled
net-connected copper zone with exact physical settings and the sixth is a distinct unfilled, locked
keepout with exact prohibited-item policy. The seventh is multiline native board text with exact
position, layer, typography, hyperlink, and lock state. The remaining five are aligned, orthogonal,
radial, leader, and center dimensions with exact native geometry, units, precision, layout policy,
labels, and text placement. Reapply groups updates by exact field mask so each dimension oneof is
updated independently. The same fixture reconciles a root and child schematic, retains the root
screen UUID and unmanaged company field, creates stable native hierarchy identities, proves the
repeat apply changes zero schematic files, injects a failed native validator and verifies exact
root-file rollback, recovers from the retained journal, checks nested named/locked group membership
on both native screens, verifies a private custom field plus complete visible Value-field rendering,
and finally exports a non-empty netlist through KiCad's real schematic loader.

Before applying the sidecar, the same smoke proof invokes KiChad's native `verify` tool against the
current-format root schematic and board. This exercises the real sibling KiCad 10.0.4 CLI rather
than an emulated rule checker: ERC must return a clean structured report, while the intentionally
incomplete pre-apply board must return its four DRC violations and one schematic-parity warning in
the correct categories. The sourcing operation has separate unit coverage against exact KDS source,
including complete physical coverage, exempt footprintless virtual components, lifecycle, stock,
freshness, malformed input, and project confinement. Normal KDS apply does not automatically run
declared checks; `fabricate.export` is the implemented production boundary that reruns ERC, DRC, and
sourcing before it generates or installs a package.

## AI source editing

KDS stays as one ordinary project-relative `.kicad_kds` file. The native `design` tool exposes
focused source operations instead of requiring every change to replace that whole file:

- `design.search` performs bounded, case-insensitive plain-text lookup and returns exact source
  windows, line/byte locations, the full-source SHA-256, and pagination state.
- `design.read` returns the whole source for compatibility or an exact bounded line range using
  `startLine` and `lineCount`.
- `design.patch` applies up to 128 ordered `{oldText, newText}` edits. Every `oldText` must occur
  exactly once in the source produced by the preceding edit, and `expectedSha256` must identify the
  current file. Combined edit text is bounded to 16 MiB.

Patch candidates are bounded, checked for embedded NUL bytes, compiled in memory, revision-checked
again immediately before commit, and atomically installed only when valid. A stale revision,
missing or ambiguous context, compilation diagnostic, or write failure leaves the original source
untouched. `design.save` is reserved for initial creation and deliberate whole-source replacement.

Run `tools/smoke-kichad-fabrication-component.sh --allow-mutation` for the complete component release
proof. The isolated harness composes only exact 10.0.4 native fixtures, controls the date on clearly
synthetic `.example.test` sourcing evidence, opens its own PCB Editor, applies and explicitly saves a routed
two-resistor design through official IPC, then runs the production exporter beside the real
`kicad-cli`. ERC and DRC must both be clean with no ignored checks, and the test verifies the exact
native footprint inventory, placement references, KDS BOM rows/MPNs, manifest release status, and
unchanged local UI settings before deleting the disposable project.

## Production support rule

`design.describe` returns the authoritative machine-readable `capabilityCoverage` inventory. It
enumerates authorable design state, execution and verification, manufacturing, interchange, editor
control surfaces, and KiCad's bundled auxiliary applications as `qualified`, `partial`, or
`unrepresented`, with the exact remaining gaps attached to every incomplete facet. This inventory
is compiler introspection for an AI model; it is not another design file or another representation
of a design. The `.kicad_kds` sidecar remains the only authored source.
The human-readable implementation summary and prioritized release roadmap are maintained in
[production-status.md](production-status.md); the runtime capability inventory remains authoritative
for individual facets.

A form is documented as executable only after it has all of the following coverage:

- parser and type-checker unit tests, including malformed and bounded-input cases;
- deterministic source-to-IR tests;
- backend unit tests with injected failures and rollback assertions;
- round-trip tests proving untouched source and KiCad data remain unchanged;
- live KiCad integration tests against disposable project copies;
- relevant ERC, DRC, sourcing, and fabrication-output assertions.

The front-end currently validates the stable identities and fields for project metadata, libraries,
components, nets, no-connect state, typed schematic groups, sourcing, board statement kinds, global
rules, net classes, custom rules, checks, and outputs. Global rules, net classes, custom rules, and
executable board forms have
backend-specific type checking and rollback coverage. Project symbol/footprint tables are
executable with native parser validation, atomic installation, journaling, and rollback coverage.
Project-local root and derived symbol content, physical and virtual/power component unit placement,
native inclusion flags, connectivity, and no-connect state are executable with lossless
reconciliation, stable identity, native netlist validation,
journaling, rollback, and a disposable live integration proof. Confined project-local footprint
instances are executable through KiCad's native parser and transaction API. Free text, text boxes,
native graphics, embedded images, complete table grids/cells, named/locked nested groups, and
complete per-unit component field rendering are executable through lossless reconciliation and
KiCad's native schematic loader. Global installed symbol content, library
content publishing, the remaining footprint/model authoring facets, and the incomplete schematic facets named by the
capability catalog remain non-executable until their own lossless backends and rollback tests land.
AI-native symbol authoring is executable for metadata, completely laid-out fields, properties,
common/numbered units, named and De Morgan body styles, unit display names, unit locking, footprint
filters, all native vector/text graphics, and fully typed pins; the capability catalog keeps its remaining authoring
facets partial until their dedicated backends land. Nested
sheet hierarchy is executable through the same transaction. Native backend
execution is enabled incrementally, and apply refuses unsupported execution before mutation.

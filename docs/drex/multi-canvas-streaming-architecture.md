# DReX Studio multi-canvas streaming architecture

Status: Experimental notes; not a product contract
Baseline: OBS Studio `7525aa1c5`
Branch: `design/multi-canvas-architecture`

## Purpose

DReX Studio shall let an operator build and monitor several independently sized canvases and route each canvas to one or more encoded video tracks in a coordinated presentation. The first target presentation contains Program Main and PixelGrid Preview, whose stable logical track number is 6.

This design keeps the DReX operator experience in the frontend, rendering and source ownership in upstream canvas objects, and publication lifecycle in a headless application service. It does not make the GUI the authority for track identity or transport assembly.

## Experiment-first constraint

This fork is currently a laboratory for discovering the operator workflow. The implementation should make the fewest irreversible choices needed to test two capabilities:

1. view and operate as many as ten canvases on one screen;
2. send as many as ten canvas-backed video tracks through one multitrack-capable RTMP output with minimal setup.

UI layouts, persistence shape, routing policy, CDN integration, ShowSync integration, and VOD behavior remain provisional. Prefer small code seams, visible diagnostics, and replaceable experimental settings over generalized frameworks. When an optimization is uncertain, first measure the simple implementation. Accepting a temporary performance cost is preferable to inventing quality policy before real operator testing.

The first preview experiment may draw the existing canvas render textures at tile size without changing canvas or stream quality. If ten simultaneous displays are too expensive, test preview-only frame-rate throttling or lower-resolution preview surfaces. Never silently lower encoded track quality as a side effect of the monitoring layout.

The current OBS output limit is `MAX_OUTPUT_VIDEO_ENCODERS = 10`. Its RTMP output and enhanced FLV muxer support multiple video encoders and explicit track IDs. This does not imply compatibility with every RTMP server; receiver preservation and playback of tracks 0 through 9 must be tested against the selected endpoint.

## Problems to solve

1. Stock OBS presents the main canvas as the primary editing context. Additional canvases exist in `libobs`, but stock OBS does not yet provide the complete multi-canvas operator workspace DReX needs.
2. Current scene-collection persistence saves additional canvas identity and flags, but not a restorable `obs_video_info`. DReX must persist and validate dimensions, pixel format, color settings, and scaling settings for every additional canvas.
3. Stock multitrack video can bind encoders to a main canvas and one selected additional canvas through service-provided configuration. DReX needs an arbitrary, explicit canvas-to-logical-track routing model that is independent of a particular streaming service API.
4. Encoder position and SDP/media order are runtime facts. They must never become the operator-facing identity of a track.
5. Starting, stopping, retrying, or partially failing a presentation must not leave an apparently live but incomplete production without an explicit policy decision.

## Scope of the first architecture

Included:

- persistent additional canvases;
- a tabbed editor with canvas-aware preview and scene docks;
- per-canvas base and output dimensions;
- stable canvas UUIDs and logical track IDs;
- one or more video encoder routes per canvas;
- coordinated validation, start, stop, and status;
- a presentation manifest suitable for ShowStream assembly;
- isolated DevCDN laboratory validation.

Deferred:

- independent frame rates per canvas, because the current canvas API uses the global rendering frame rate;
- seamless movement of a scene between live canvases;
- arbitrary sharing of scene objects across canvases;
- adaptive bitrate policy and automatic rendition generation;
- production StreamForge policy or control-plane behavior;
- migration of the known-good `/pixelgrid` publishing path.

## Upstream capabilities we will reuse

The baseline already provides:

- reference-counted `obs_canvas_t` objects with stable UUIDs;
- a separate source and scene registry for each canvas;
- a video mix and `video_t` for each configured canvas;
- canvas add, remove, enumerate, rename, reset, render, and signal APIs;
- scene-collection storage for additional canvases and each scene's `canvas_uuid`;
- frontend events for canvas addition and removal;
- encoder binding with `obs_encoder_set_video(..., obs_canvas_get_video(canvas))`;
- multitrack encoder configurations containing a canvas index.

The Canvas API is explicitly marked unstable upstream. DReX-specific use of it must be concentrated behind small frontend and application-model adapters so an upstream API change does not spread throughout the fork.

## Architectural boundaries

```text
Qt operator workspace
  - canvas tabs and tiled monitoring
  - canvas-aware scene/source editing
  - routing and validation views
             |
             v
DReX presentation application model
  - owns desired configuration and runtime state
  - resolves UUIDs and logical IDs
  - validates the complete presentation
  - coordinates start and stop
        |                         |
        v                         v
OBS canvas adapter          DReX output adapter
  - obs_canvas_t              - encoder creation
  - scenes and sources        - logical-to-physical mapping
  - video mix                 - publisher/assembler contract
        |                         |
        +------------+------------+
                     v
            observed presentation state
            and versioned manifest
```

The Qt widgets issue commands to the application model and render its state. They do not create encoders, assign physical track indices, or directly own publisher processes.

## Persistent project model

DReX data will be stored as one versioned object in the scene collection. The namespace must be unique and optional so upstream fields remain intact:

```json
{
  "drex_studio": {
    "schema_version": 1,
    "canvases": [
      {
        "canvas_uuid": "uuid-from-libobs",
        "role": "program-main",
        "base_width": 1920,
        "base_height": 1080,
        "output_width": 1920,
        "output_height": 1080,
        "format": "NV12",
        "colorspace": "709",
        "range": "partial"
      }
    ],
    "presentations": [
      {
        "presentation_id": "stageflight-preproduction",
        "revision": 1,
        "routes": [
          {
            "route_id": "program-main-h264",
            "canvas_uuid": "uuid-from-libobs",
            "logical_track_id": "program-main",
            "logical_track_number": 1,
            "encoder_profile_id": "h264-program"
          },
          {
            "route_id": "pixelgrid-preview-h264",
            "canvas_uuid": "another-canvas-uuid",
            "logical_track_id": "pixelgrid-preview",
            "logical_track_number": 6,
            "encoder_profile_id": "h264-pixelgrid-preview"
          }
        ]
      }
    ]
  }
}
```

The example is descriptive, not yet a frozen interchange schema. Encoder secrets and destination credentials must not be stored in the scene collection.

### Identity rules

- Canvas UUID is the persistent rendering identity.
- `logical_track_id` is the stable machine-facing presentation identity.
- `logical_track_number` is stable operator-facing metadata.
- `route_id` identifies one encoding of one canvas.
- Physical encoder index and transport media index are observed runtime mappings only.
- Renaming a canvas or track does not change any identity.
- A missing canvas UUID is a validation error; it is not silently rebound by name.

## Canvas ownership and editing

Each scene and group belongs to exactly one canvas, following upstream canvas ownership. The first implementation will not pretend that a scene can be edited concurrently in several coordinate spaces.

The active canvas determines:

- the preview/editor surface;
- the scene list;
- scene creation destination;
- transform coordinate space;
- base and output size shown by video settings;
- applicable track routes shown by the routing dock.

Input sources that can safely be reused may be referenced by scenes on different canvases through normal OBS source semantics. Cross-canvas scene nesting and linked transforms require a later explicit design because activation, audio, ownership, and deletion behavior must be proven first.

## Video configuration

DReX will create every additional canvas with an explicit `obs_video_info` and persist the relevant fields in `drex_studio.canvases`. During scene-collection load it will:

1. let upstream restore canvas UUIDs and flags;
2. read and validate the DReX video descriptor;
3. reconstruct an `obs_video_info` using the current global FPS;
4. call `obs_canvas_reset_video` before an output may use the canvas;
5. report invalid or unsupported settings without silently substituting the main canvas.

Changing a canvas video configuration is prohibited while video outputs are active, matching the current `libobs` constraint. The UI must explain which active output blocks the change.

## Routing and output lifecycle

The presentation application model has desired and observed state.

Desired state contains canvas descriptors, logical routes, encoder profiles, presentation ID, and failure policy. Observed state contains resolved canvas references, encoder instances, physical encoder indices, observed transport indices, output status, and errors.

Start is transactional at the application-model boundary:

1. snapshot desired configuration revision;
2. resolve every canvas UUID;
3. validate video mixes and encoder availability;
4. validate duplicate logical IDs and track numbers;
5. create all encoders and bind each to its canvas video;
6. configure the presentation output or assembler;
7. publish the manifest and observed mapping;
8. start the output;
9. mark the presentation running only after required tracks are observed.

If a required route fails before start, nothing starts. Runtime partial failure defaults to stopping the coordinated presentation and reporting the failed route. A future manifest may explicitly mark a route optional, but optionality must never be inferred from track order or canvas name.

Stop is idempotent and releases outputs, encoder groups, encoders, and retained canvas references through one application-model operation.

## Relationship to stock multitrack video

The existing `MultitrackVideoOutput` proves that multiple encoders can bind to different canvas video objects. It is not the DReX domain model because:

- its configuration is centered on the current Go Live service response;
- the frontend setting currently selects only one extra canvas UUID;
- encoder configuration uses a positional `canvas_index`;
- it does not provide stable DReX logical track identity or the ShowStream presentation contract.

DReX should initially reuse its low-level encoder binding patterns, not modify its service behavior. General-purpose DReX routing will live behind a separate output adapter. Reusable improvements that are not DReX-specific can later be proposed upstream.

## ShowStream and DevCDN boundary

DReX Studio produces encoded routes plus a versioned desired manifest. ShowStream remains responsible for assembling independent sources into the final multitrack presentation and reporting the physical mapping it actually observed.

The first live proof uses the isolated preproduction presentation. The known-good `/pixelgrid` path remains unchanged until the new path has demonstrated:

- Program Main and PixelGrid Preview are both present;
- PixelGrid retains logical track number 6 regardless of media order;
- codec and timing constraints are satisfied;
- start, stop, reconnect, and failure states are observable;
- the presentation survives inspection by real media tools and the intended client.

DevCDN receives the assembled presentation and observations. It does not become authoritative for canvas layout, logical track ownership, or production routing policy.

## Compatibility and recovery

- DReX Studio preserves upstream scene-collection fields and stores extensions under a versioned namespace.
- Loading a DReX collection in a build that lacks the required canvases can cause upstream to fall affected scenes back to the main canvas. Therefore opening a production DReX collection in stock or older OBS is not considered a safe round trip.
- Before format-changing migrations, DReX creates a recoverable scene-collection backup and increments the DReX schema version only after a successful write.
- Unknown newer DReX schema versions fail closed and remain unmodified.
- A future export command may flatten one selected canvas into a stock-compatible collection; that is separate from normal save.
- Existing stock collections with no `drex_studio` object continue to open as one main canvas without migration.

## Performance and observability

Each configured canvas adds a render mix, and each route may add scaling and encoding work. The first implementation records per-canvas render time, skipped frames, encoder utilization, output congestion, and route state.

The UI must distinguish:

- canvas is configured;
- canvas is rendering;
- encoder is active;
- output accepted the encoder;
- required track was observed in the published presentation.

Structural success is not live media proof.

## Implementation sequence

### Slice 1: persistent canvas model

- add a DReX project-model class and versioned serializer;
- create, rename, remove, save, load, and reset one additional canvas;
- add round-trip tests for UUID and video settings;
- add backup and unknown-version behavior.

### Slice 2: operator workspace

- add canvas tabs above the editor;
- switch preview and scene context by active canvas;
- make scene creation canvas-aware;
- show canvas resolution and invalid state;
- prove save/restart/restore with Main and PixelGrid Preview.

### Slice 3: route model and local encoder proof

- add stable logical route configuration;
- bind two encoders to two canvases;
- write an inspectable local artifact or test output;
- verify physical ordering cannot change logical identity.

### Slice 4: ShowStream adapter

- emit desired presentation manifest;
- register encoded routes with the assembler;
- receive and display observed mapping;
- test only on the isolated preproduction presentation.

### Slice 5: operational hardening

- transactional start and idempotent stop;
- runtime partial-failure policy;
- load, GPU, timing, reconnect, and soak tests;
- packaging and upgrade validation against a fresh upstream merge.

## Acceptance criteria for the first code slice

1. An untouched stock scene collection opens without migration.
2. The operator can create a named additional canvas with an independent resolution.
3. A scene created in that canvas retains its canvas UUID after restart.
4. The additional canvas video mix is restored and renders after restart.
5. Removing the canvas requires an explicit choice for its scenes and routes.
6. Missing or invalid canvas video data is reported; it is not silently mapped to Main.
7. No encoder, publisher, DevCDN, or ShowStream behavior is changed in this slice.
8. The stock main-canvas build and output smoke tests still pass.

## Open decisions before Slice 1 implementation

1. Whether the DReX extension belongs directly at `drex_studio` or within the existing scene-collection `modules` object.
2. Whether deleting a canvas initially blocks when it owns scenes, or offers an explicit move-to-Main operation.
3. Which `obs_video_info` fields are user-configurable versus derived from global video settings.
4. Whether Main is represented explicitly in the DReX canvas descriptors or synthesized when absent.
5. The initial required-track failure policy for local encoder proof: stop-all or keep-running-with-error. The recommended default is stop-all.

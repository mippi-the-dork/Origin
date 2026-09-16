# Origin

Origin adds adjustable assembly pivots and hierarchy selection controls to Unreal Engine's World Outliner. Create an **Origin Anchor** above a selection, then move, rotate or scale the actors together. Change the anchor's pivot without changing its children's world transforms.

**Editor compatibility:** Unreal Engine 5.8.2 on Windows (64-bit).

**Packaged-game support:** Designed for packaged games; not yet verified. Origin Anchors are runtime actors, so keep Origin enabled while your levels use them.

## Features

- Create an Origin Anchor from the Outliner toolbar or actor context menu.
- Choose from nine pivot modes, with optional world-space or anchor-local offsets.
- Reposition an anchor while preserving its children's world location, rotation and scale.
- Select parents, siblings, immediate children or all descendants directly from Outliner rows.
- Release children or convert an anchor to a native empty Actor.
- Undo/Redo for creation, pivot settings, pivot changes and removal operations.
- Save pivot settings with the level.
- Respect Focus Transform/Edit Locks without requiring Focus to be installed.

Origin requires no engine modifications or Motion Design dependency. It works independently of Focus and Surface.

## Installation

### From a precompiled release ZIP

1. Close Unreal Editor.
2. Download the Origin plugin ZIP from the GitHub release's **Assets** section.
3. Extract the `Origin` folder into your project's `Plugins` folder. Create `Plugins` if it does not exist.
4. Confirm the descriptor is at `YourProject/Plugins/Origin/Origin.uplugin`.
5. Open your project and enable **Origin** under **Edit > Plugins**.
6. Restart the editor if prompted.

Use a release built for your Unreal Engine version and platform. GitHub's automatically generated **Source code** downloads are not precompiled plugin packages.

### From source

1. Place `Origin` in your C++ project's `Plugins` folder and close Unreal Editor.
2. Right-click the `.uproject` file and choose **Generate Visual Studio project files**.
3. Open the solution, select **Development Editor / Win64**, and build your project.
4. Open Unreal, enable Origin and restart if prompted.

Source builds require the C++ toolchain supported by your Unreal installation. Changes to saved actor fields should be built with the editor closed rather than through Live Coding.

## Create an assembly

Select actors and click the **Add Origin Anchor** icon beside the World Outliner search controls. Unreal's existing folder button remains available. You can also use **Create Origin Anchor** in the actor context menu.

The anchor starts at the combined bounds center. Selected actors already beneath another selected actor retain that nested relationship. With no actors selected, Origin creates an empty anchor at world origin.

Move, rotate or scale the anchor to manipulate its attached assembly. Normal Unreal attachment behavior applies, including component absolute-transform settings.

The command has no default keyboard shortcut. Assign one under **Editor Preferences > Keyboard Shortcuts > Origin**.

## Pivot controls

Select an Origin Anchor and expand **Origin** in its Details panel. The section appears below Transform and above Surface's sections when Surface is installed. Controls appear only when their selected mode needs them.

| Pivot mode | Position used |
| --- | --- |
| Bounds Center | Center of the combined world-space bounds of children and descendants. |
| Average Child Pivots | Average world position of direct children, with equal weight for each actor. |
| Bottom Center | Combined bounds center in X/Y, at the lowest world-space Z. |
| Child Actor | World position of a chosen direct child actor. |
| World Origin | World position 0, 0, 0. |
| Custom | Manually entered world-space X/Y/Z coordinates. |
| Bounds Point | A face center, edge, corner or center chosen using Min/Center/Max for each world axis. Presets provide common face-center positions. |
| Reference Actor | World position of a loaded actor outside this assembly, selected through the picker or eyedropper. |
| Component / Socket | World position of a scene component or named socket on a chosen direct child. |

Bounds include registered primitive components except editor visualization components. Empty actors contribute their actor position. Child Actor and Reference Actor use actor locations; they do not copy rotation or temporary viewport pivot offsets.

Choosing valid settings applies the pivot immediately. The anchor keeps its rotation and scale, and children retain their world transforms. Incomplete or missing source selections leave the assembly stationary until a valid source is available.

### Pivot Offset

Expand **Pivot Offset** to add an offset to any pivot mode.

- **World:** Coordinates follow world axes.
- **Anchor Local:** Coordinates follow the anchor's rotation. Anchor scale does not scale the offset distance.
- **Offset X / Y / Z:** Distances are in centimeters.
- **Reset Offset:** Returns all three coordinates to zero while retaining the selected space.

### Recalculate Pivot

Origin does not continuously recenter an assembly. Use **Recalculate Pivot** after its source changes, such as moving a child, reference actor or source component.

The button is disabled when the pivot is already up to date, a required source is unavailable, or an editing restriction prevents the operation. Hover over the button area to read the reason. A status message also appears in the panel. Changes that would produce the same pivot position do not enable the button.

For example, move a child outward while using Bounds Center, then recalculate to center the anchor on the updated assembly without moving its children.

## Outliner selection controls

The **Origin** column appears after Item Label. It can be resized or hidden through the Outliner's column controls.

| Control | Selection |
| --- | --- |
| Square, top half | Immediate children. |
| Square, bottom half | All descendants. |
| Circle, left half | Parent actor or folder. |
| Circle, right half | Siblings, excluding the clicked row. |

Click replaces the selection; **Shift-click** adds to it. Each action uses the clicked row, even when other rows are selected. Unavailable actions are dimmed.

These controls work with ordinary actor attachments and folders as well as Origin Anchors. Selecting a folder selects its row, not every actor inside it. The world root is not offered as a selectable parent, and native actor groups are not treated as attachment parents.

Only loaded actors are included. Actor selection does not depend on branches being expanded. Clear Outliner filters if you also need to select folder rows hidden by those filters.

## Release or convert an anchor

- **Release Children:** Detaches direct children to the world and removes the anchor. Their world transforms and deeper child attachments are preserved.
- **Convert to Empty Actor:** Replaces the anchor with a native empty Actor with a scene root. The assembly, transform, label and folder are retained; Origin pivot settings are removed.
- **Convert Selected Origin Anchors:** Converts multiple selected, non-nested anchors from the context menu. Process nested anchors separately, deepest first.

These operations support Undo and require a plain Origin Anchor with only its original scene root. Detected persistent references can block removal. Worlds containing a Level Sequence Actor are also blocked by the current removal checks. Sequence bindings and unloaded or custom reference systems require separate review.

Before disabling or uninstalling Origin, remove or convert its anchors in **every relevant level**, including actors in unloaded World Partition cells, and save those levels. The conversion command only processes the selected loaded anchors; it is not a project-wide migration.

## Runtime behavior

Origin Anchor is a real actor in the `Origin` Runtime module. Its scene root and attachment hierarchy are designed to remain in a packaged game. The `OriginEditor` module provides the authoring controls. Pivot settings are editor-only; there is no automatic runtime pivot recalculation.

An anchor has no tick, rendered geometry or collision. Creation uses Static mobility when a selected direct child is Static, and Movable otherwise. Child mobility is not changed. Gameplay movement requires a compatible Movable assembly.

**A game-target build, cooking and packaged execution have not yet been verified.** Successful editor testing does not establish packaged-game compatibility.

## Editing limits and compatibility

- Origin's authoring operations are unavailable during PIE or simulation.
- Origin does not load World Partition cells automatically.
- Creation requires the selected top-level actors to share a level and immediate attachment parent/component/socket.
- Locked levels, protected actors, zero-scale transforms and physics-simulating roots prevent assembly changes.
- Ungroup native actor groups before creating or restructuring an Origin assembly.
- Creation beneath non-uniformly scaled ancestors is not supported.
- Origin reads Focus's saved Transform/Edit Lock flags and Unreal's movement lock. Relevant ancestor or descendant locks can block operations even if the anchor itself is unlocked. Selection Lock does not block Outliner hierarchy selection.
- Another plugin that replaces the standard Outliner browsing mode may conflict with the toolbar integration.

The editor workflows, Release Children and Convert to Empty Actor have been manually tested by the author. Broader reference migration and World Partition streaming scenarios are not covered by those tests.

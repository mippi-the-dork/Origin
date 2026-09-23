# Origin implementation sources

Origin uses Unreal Engine APIs without engine modifications. The Unreal Engine 5.8 source files below informed its implementation. These engine files are not redistributed with the plugin.

## Outliner integration

| Engine source | Use in Origin |
| --- | --- |
| `SceneOutlinerModule.h/.cpp` | Actor-browser initialization, custom columns and mode factory. |
| `ActorBrowsingMode.h`, `ISceneOutlinerMode.h`, `SSceneOutliner.cpp` | Add Anchor toolbar control while retaining standard browsing behavior and the folder button. |
| `ISceneOutlinerColumn.h`, `ActorTreeItem.h`, `FolderTreeItem.h`, `SceneOutlinerStandaloneTypes.h` | Hierarchy selection controls and actor/folder identities. |
| `SceneOutlinerDragDrop.h`, `ISceneOutlinerMode.h` | Native drag/drop parsing and validation used to classify parent and deparent previews. |
| `SWindow.h`, `SBorder.h`, `SlateApplication.h` | Input-transparent edge-strip hierarchy highlight overlays that leave Unreal's native row content and styling uncovered. |
| `ActorModeInteractive.cpp` | Actor-selection notifications and their effect on folder-row selection. |
| `SLevelEditor.cpp`, `LevelEditorContextMenu.cpp` | Standard Outliner creation and Level Editor context-menu integration. |
| `EditorActorFolders.h` | Folder enumeration. |
| `SButton.h/.cpp` | Explicit button padding and geometry for the split selection controls. |

## Pivot and actor operations

| Engine source | Use in Origin |
| --- | --- |
| `Actor.h`, `ActorEditor.cpp`, `SceneComponent.h/.cpp` | Actor attachments, world transforms, mobility, scene-component and socket positions. |
| `EditorSelectUtils.cpp`, `UnrealEdEngine.h` | Refresh the viewport's cached selection pivot after repositioning an anchor. |
| `EditorActorSubsystem.h/.cpp`, `ActorFactoryEmptyActor.h` | Replace an anchor with a native empty Actor using Unreal's replacement path. |
| `EditorActor.cpp` | Soft-reference queries through AssetTools before anchor removal. |
| `ScopedTransaction.h` | Undo/Redo transactions for authoring operations. |
| `MetaData.h/.cpp`, `Package.h/.cpp` | Read Focus's saved lock state without a module dependency. |

## Details panel

| Engine source | Use in Origin |
| --- | --- |
| `DetailLayoutBuilder.h`, `DetailCategoryBuilder.h`, `DetailWidgetRow.h`, `IDetailGroup.h` | Origin category, conditional rows, category ordering and the collapsed Pivot Offset group. |
| `SDetailCategoryTableRow.cpp` | Category-header icon and label while retaining Unreal's expander. |
| `PropertyCustomizationHelpers.h/.cpp`, `SPropertyEditorAsset.cpp` | Reference Actor picker, actor filtering and eyedropper. |

## Project settings

Origin exposes hierarchy highlight colors through `UDeveloperSettings` under **Project Settings > Plugins > Origin > Appearance > Hierarchy**. The settings are editor-only and affect drag-preview rendering; they do not change hierarchy mutation behavior.

Hierarchy feedback is independent of Index. If the Index module is loaded, Origin reads Index's editor config at runtime and suppresses hierarchy outlines inside Index's configured reorder edge zones or while Index's force-reorder modifier is held. There is no compile-time or plugin dependency on Index.

## Optional Focus integration

Origin reads the metadata key `Focus.SharedState.V1`. Its five binary characters represent Selection Lock, Transform Lock, Edit Lock, Solo and editor hidden state. Origin uses the Transform Lock and Edit Lock values when checking whether an assembly can be changed.

Origin does not link to Focus or modify this metadata. Changes to Focus's storage format may require an interoperability update. Surface is also optional; Origin orders its category ahead of Surface's categories when they are present.

## Artwork and design references

Mippi supplied the anchor, Add Anchor and hierarchy-selection SVGs, along with the plugin PNG icon. The artwork is retained unchanged. The plugin browser image is named `Resources/Icon128.png`; Slate controls the SVG display sizes and button spacing.

Motion Design's Outliner informed the toolbar interaction. Origin does not require Motion Design mode or its modules, and it does not include Actor Locker implementation files.

## Testing scope

The author has confirmed the editor workflows, Release Children and Convert to Empty Actor through manual testing. A game-target build, cooking and packaged execution remain unverified. The Runtime/Editor module separation is intended to support packaged games, but is not itself proof of successful packaging.

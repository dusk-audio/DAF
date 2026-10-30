# Native file-drag tests

The default standalone build tests Windows OLE file payloads or Cocoa gesture capture without injecting desktop input:

```sh
cmake -S tests/native-file-drag -B build-file-drag
cmake --build build-file-drag --config Release
ctest --test-dir build-file-drag -C Release --output-on-failure
```

Enable `-DDAF_FILE_DRAG_INTERACTIVE=ON` to add a real native destination test. Run it on an unlocked desktop with no mouse button held. It creates its own source and destination windows, activates its source before injecting a gesture, restores the cursor and foreground application, and removes its fixture. Windows service session 0 cannot run this test. Cocoa requires existing event-posting permission; the test checks access and never requests or changes it.

On macOS it also adds `file-drop-native`, where DGL windows that accept drops receive DGL's own drag: one takes it with Option held and deletes itself inside `onFileDrop`, one refuses, and the source must be told copy, then none. Where the test may not post events, `file-drop-native --external 90` waits for the same two drags from a remote desktop's input and prints where to make them; `file-drop-native --wait 90` is a window to drag Finder files onto by hand.

On Windows the default build also tests the OLE drop target of a DGL window that accepts file drops (`FileDropWin.cpp`), calling its `IDropTarget` methods with a fake data object; it needs a desktop session but no input. The interactive build adds `FileDropWinNative.cpp`, which drags onto a DGL window that accepts drops: from a DGL window of the same process, then from a second process, the test program run again as a plain OLE source.

The Windows destination destroys the real DGL Window and Application before retrieving CF_HDROP. The Cocoa destination destroys its native source view before reading the file URL. Both verify Unicode paths and original file contents after closure. Cocoa compares complete paths under canonical Unicode normalization because its native pasteboard can return decomposed filenames.

Linux integration is exercised by the Chord Analyzer real-CLAP editor gate, including XDND selection transfer, exact MIDI bytes, cancellation, and source closure.

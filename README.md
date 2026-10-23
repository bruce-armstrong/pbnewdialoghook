# pbnewdialoghook

Adds your own entries to the **File → New** dialog in the PowerBuilder 2025 R2
(PB250) IDE. Each entry shows up in the object-type list — with its own icon —
alongside the built-in types (Window, Menu, DataWindow, …) on a category tab you
choose. Activating one (double-click or **OK**) runs a command of your choice.

The entries, their icons, actions, and logging are all driven by a plain
`pbnewdialoghook.ini` file — no rebuild needed to change them.

> **Note:** The PowerBuilder IDE has no official add-in API, so pbnewdialoghook works
> by injecting a small DLL into the IDE process using documented Windows hooks.
> This is unsupported by Appeon. See [Caveats](#caveats).

---

## Files

| File            | Purpose                                                       |
|-----------------|---------------------------------------------------------------|
| `pbnewdialoghook.dll` | The hook. Injected into the IDE; adds the list entries.       |
| `pbnewdialoghookload.exe`    | Loader. Installs the hook into the running IDE.               |
| `pbnewdialoghook.ini` | Configuration: your entries, icons, actions, and logging.     |
| `pbnewdialoghook.c`   | Source for the DLL.                                           |
| `pbnewdialoghookload.c`      | Source for the loader.                                        |
| `build.cmd`     | Rebuilds `pbnewdialoghook.dll` and `pbnewdialoghookload.exe`.                    |
| `LICENSE`       | MIT License.                                                  |

## Requirements

- **Windows** with the **PowerBuilder 2025 R2 IDE** (`PB250.exe`).
- The hook and loader are **32-bit (x86)** because the IDE is a 32-bit process.
  The prebuilt binaries are ready to use; you only need a compiler to rebuild.

## Usage

1. Keep `pbnewdialoghook.dll` and `pbnewdialoghook.ini` together in the same folder.
2. Start the PowerBuilder IDE and open a workspace/target.
3. Run `pbnewdialoghookload.exe`. A message box says **"Hook installed."**
   **Leave it open** — the hook is active only while the loader is running.
4. Open **File → New**. On the configured category tab (default **PB Object**),
   your entries appear at the end of the object list with their icons.
5. Double-click an entry, or select it and click **OK**, to run its action. The
   dialog then closes (the IDE does *not* create a built-in object for it).
6. To remove the hook, click **OK** on the loader's message box *before* closing
   the IDE.

## Configuration (`pbnewdialoghook.ini`)

`pbnewdialoghook.ini` must sit next to `pbnewdialoghook.dll`. It is re-read every time the
object list is (re)populated — including when you switch category tabs — so
edits take effect immediately, with no need to restart the loader or the IDE.

```ini
[pbnewdialoghook]
Items=2
Tab=PB Object
Log=0
; LogFile=C:\Users\me\Downloads\pbnewdialoghook\pbnewdialoghook.log

[Item1]
Label=My template object
Icon=C:\Tools\mine.ico
Command=C:\Tools\mytool.exe
Args="%t"
; Dir=C:\Tools
; Tab=PB Object

[Item2]
Label=Show current target
Command=:msgbox
Args=Target: %t%nName:   %T
```

### `[pbnewdialoghook]` section

| Key       | Meaning                                                              |
|-----------|---------------------------------------------------------------------|
| `Items`   | Number of entries to read (`[Item1]` … `[ItemN]`).                  |
| `Tab`     | Default category tab for entries without their own `Tab=`.           |
| `Log`     | `1` turns on the debug log, `0` off.                                 |
| `LogFile` | Optional log path. If omitted, `pbnewdialoghook.log` is written by the DLL. |

### `[ItemN]` sections

| Key       | Meaning                                                                      |
|-----------|------------------------------------------------------------------------------|
| `Label`   | The text shown in the object list.                                            |
| `Icon`    | Optional path to an `.ico` shown next to the entry.                           |
| `Command` | A program to run, **or** a built-in action (see below).                       |
| `Args`    | Command-line arguments passed to the program.                                 |
| `Dir`     | Optional working directory for the program.                                   |
| `Tab`     | Optional category tab for this entry (overrides the default `Tab`).           |

**Category tabs** in the New dialog (target-dependent): `PB Object`,
`DataWindow`, `Database`, `Project`, `Project Object`, `Library`, `Tool`.
Entries attach to the object **icon list**, so target one of these tabs. The
`Solution` / `Workspace` page presents big buttons instead of the icon list, so
it has nowhere to put entries — don't target it.

**Built-in actions** (use as `Command`):

| Command      | Effect                                  |
|--------------|-----------------------------------------|
| `:clipboard` | Copies `Args` to the Windows clipboard. |
| `:msgbox`    | Shows `Args` in a message box.          |

Anything else in `Command` is launched with `ShellExecute` (`.exe`, `.bat`,
`.cmd`, a document, etc.).

### Placeholders

Usable in `Label` and `Args`:

| Token | Expands to                                                                      |
|-------|---------------------------------------------------------------------------------|
| `%t`  | Current target, as shown in the **Target:** box, e.g. `peat demo app (C:\…\…)`   |
| `%T`  | Target **name only**, without the ` (path)` suffix, e.g. `peat demo app`        |
| `%n`  | Newline (handy in `:msgbox`)                                                     |
| `%%`  | A literal percent sign                                                          |

> Save `pbnewdialoghook.ini` as **ANSI** or **UTF-16** (Notepad's default is fine).

## Logging

Set `Log=1` in `[pbnewdialoghook]` to trace what the hook does — config loads, each
injection (which tab, how many items), icons added, and every action fired
(including `ShellExecute` failures). The log defaults to `pbnewdialoghook.log` next to
the DLL, or set `LogFile=` to another path. Set `Log=0` to turn it off.

## How it works

The File → New dialog is a standard Win32 dialog (`#32770`). Its object types
live in a normal `SysListView32` (control id **1031**) and its categories in a
`SysTabControl32` (id **400**). pbnewdialoghook subclasses the dialog, appends list
items with an icon added to the list's image list, and re-appends them whenever a
tab change repopulates the list.

The IDE identifies the selected object type by each list item's `lParam` (an
internal pointer it dereferences). Our items instead carry a tagged `lParam`
(`0x6E48xxxx`) that encodes the config index, and the dialog subclass intercepts
**OK** / double-click on our items *before* the IDE reads the selection — so the
IDE never dereferences our tag. Command ids and the `lParam` tag avoid the IDE's
own ranges.

## Building

The binaries ship ready to use. To rebuild, install a 32-bit MinGW-w64 toolchain
and run `build.cmd`.

1. Install [MSYS2](https://www.msys2.org/), then from the **MSYS2 MINGW32**
   shell:
   ```
   pacman -S --needed mingw-w64-i686-gcc
   ```
2. Put `C:\msys64\mingw32\bin` on your `PATH` (or build from that shell).
3. From this folder:
   ```
   build.cmd
   ```

`pbnewdialoghook.dll` must not be loaded while building — close the loader first, or
the build will fail with "Permission denied" on the DLL.

## Caveats

- **Unsupported.** PowerBuilder has no public IDE add-in API. pbnewdialoghook uses
  Windows message hooks and injection; it may need re-checking after any
  PowerBuilder update.
- **Antivirus.** DLL injection is exactly what antivirus software watches for.
  You will likely need to allow the files or add an exclusion, and to
  **code-sign** the DLL if you distribute it to other machines.
- **Bitness.** The IDE is 32-bit, so the DLL and loader must be 32-bit.

## License

Copyright (c) 2026 Bruce Armstrong

Released under the MIT License. See [LICENSE](LICENSE) for the full text.

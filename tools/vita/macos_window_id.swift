// Prints the CGWindowID of the largest normal window owned by a process id (macOS only).
//   swift tools/vita/macos_window_id.swift <pid>      exits 1 and prints nothing if there is none
// Used by vita3k_macos.sh --screenshot so `screencapture -l <id>` grabs only that window.
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: macos_window_id.swift <pid>\n".data(using: .utf8)!)
    exit(2)
}
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
var best: (id: Int, area: Double)?
for w in list {
    guard (w[kCGWindowOwnerPID as String] as? Int32) == pid,
          (w[kCGWindowLayer as String] as? Int) == 0,
          let id = w[kCGWindowNumber as String] as? Int,
          let b = w[kCGWindowBounds as String] as? [String: Double],
          let width = b["Width"], let height = b["Height"], width > 100, height > 100 else { continue }
    if best == nil || width * height > best!.area { best = (id, width * height) }
}
guard let found = best else { exit(1) }
print(found.id)

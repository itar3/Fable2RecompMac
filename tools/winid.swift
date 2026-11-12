// Prints the window id of the largest window of <pid> (or of any
// process named Fable2 without an argument), for screencapture -l<id>.
import CoreGraphics

let want: Int? = CommandLine.arguments.count > 1 ? Int(CommandLine.arguments[1]) : nil
// All windows, not only on-screen ones: the game window may sit in another
// Space (screencapture -l still captures it).
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
var best: (id: Int, area: Double)? = nil
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    let pid = w[kCGWindowOwnerPID as String] as? Int ?? -1
    guard want.map({ $0 == pid }) ?? owner.contains("Fable2") else { continue }
    guard (w[kCGWindowLayer as String] as? Int ?? 0) == 0,
          let b = w[kCGWindowBounds as String] as? [String: Double], let id = w[kCGWindowNumber as String] as? Int
    else { continue }
    let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
    if best == nil || area > best!.area { best = (id, area) }
}
if let b = best { print(b.id) } else { exit(1) }

// Sends key presses to the Fable2 window: keys <pid> <keycode:ms[:pauseMs]> ...
// Each item holds the key for ms milliseconds, then waits pauseMs (default 150).
// Keys are sent only while a window of <pid> is the frontmost window, so they
// never land in another app: exits with status 2 when focus is lost.
// macOS key codes: Space 49, Return 36, W 13, A 0, S 1, D 2, arrows 123-126.
import AppKit
import CoreGraphics

let args = CommandLine.arguments
guard args.count >= 3, let pid = Int32(args[1]) else {
    print("usage: keys <pid> <keycode:ms[:pause]> ...")
    exit(1)
}

func frontmostPid() -> Int32? {
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
        as? [[String: Any]] ?? []
    for w in list where (w[kCGWindowLayer as String] as? Int) == 0 {
        return (w[kCGWindowOwnerPID as String] as? Int).map { Int32($0) }
    }
    return nil
}

func ensureFront() -> Bool {
    if frontmostPid() == pid { return true }
    NSRunningApplication(processIdentifier: pid)?.activate(options: [])
    for _ in 0..<20 {
        usleep(100_000)
        if frontmostPid() == pid { return true }
    }
    return false
}

guard ensureFront() else {
    print("keys: Fable2 (pid \(pid)) is not the frontmost window")
    exit(2)
}
usleep(200_000)
let source = CGEventSource(stateID: .hidSystemState)
for item in args.dropFirst(2) {
    let parts = item.split(separator: ":").compactMap { Int($0) }
    guard parts.count >= 2 else { continue }
    guard ensureFront() else {
        print("keys: focus lost before \(item)")
        exit(2)
    }
    let key = CGKeyCode(parts[0])
    let hold = parts[1]
    let pause = parts.count >= 3 ? parts[2] : 150
    CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: true)?.post(tap: .cghidEventTap)
    usleep(useconds_t(hold * 1000))
    CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: false)?.post(tap: .cghidEventTap)
    usleep(useconds_t(pause * 1000))
}

// Fable 2 launcher: a settings window with a Play button that starts the game
// with the chosen options, "Install from ISO" that sets the game files up from
// a disc image, and a title update row that finds and installs title update 1.
// Built into "Fable 2.app" next to the game by CMake (cmake/launcher.cmake);
// the game, assets and shader dump folders and the fingerprint of the program
// the build was recompiled from come from the bundle's Info.plist (set at build
// time), so the app can be moved to /Applications or the Dock. Choices are kept
// in the app's user defaults. Started with --play, it starts the game at once
// with the saved choices; --update-location <path> and --install-iso <path> do
// what the buttons do, without dialogs.
//
// A disc is installed only when its program, with title update 1 applied (or
// built in), loads into exactly the image this build was recompiled from
// (fable2-extract --image-hash): the media ID and version labels differ between
// discs whose code is the same.

import AppKit
import CryptoKit
import SwiftUI
import UniformTypeIdentifiers

// MARK: - Settings

enum DisplayMode: String, CaseIterable, Identifiable {
  case windowed, fullscreen
  var id: String { rawValue }
  var label: String { self == .windowed ? "Windowed" : "Full screen" }
}

struct WindowSize: Identifiable, Hashable {
  let width: Int, height: Int
  var id: String { "\(width)x\(height)" }
  static let all = [WindowSize(width: 1280, height: 720), WindowSize(width: 1600, height: 900),
                    WindowSize(width: 1920, height: 1080), WindowSize(width: 2560, height: 1440)]
}

let resolutions: [(value: String, label: String)] = [
  ("720p", "720p"), ("1080p", "1080p"), ("1440p", "1440p"), ("2160p", "2160p (4K)"),
  ("display", "Match the display"),
]
let renderScales: [(value: Double, label: String)] = [
  (0.5, "50%"), (0.75, "75%"), (1.0, "100%"), (1.25, "125%"), (1.5, "150%"),
]
let frameCaps: [(value: Int, label: String)] = [(0, "Off"), (30, "30 fps"), (60, "60 fps")]

let fable2TitleId: UInt32 = 0x4D53_07F1

// MARK: - Paths

struct GamePaths {
  let gameDir: URL
  let assetsDir: URL
  let shaderDumpDir: URL?
  let expectedImageHash: String?
  let expectedImageLabel: String

  static func load() -> GamePaths {
    let info = Bundle.main.infoDictionary ?? [:]
    let bundleParent = Bundle.main.bundleURL.deletingLastPathComponent()
    func string(_ key: String) -> String? {
      guard let value = info[key] as? String, !value.isEmpty, !value.hasPrefix("@") else { return nil }
      return value
    }
    func dir(_ key: String) -> URL? { string(key).map { URL(fileURLWithPath: $0, isDirectory: true) } }
    let game = dir("Fable2GameDir") ?? bundleParent
    return GamePaths(gameDir: game, assetsDir: dir("Fable2AssetsDir") ?? game.appendingPathComponent("assets"),
                     shaderDumpDir: dir("Fable2ShaderDumpDir"),
                     expectedImageHash: string("Fable2GameImageHash")?.lowercased(),
                     expectedImageLabel: string("Fable2GameImageLabel") ?? "the program this build was made from")
  }

  var executable: URL { gameDir.appendingPathComponent("Fable2") }
  var extractor: URL { gameDir.appendingPathComponent("fable2-extract") }
  var logsDir: URL { gameDir.appendingPathComponent("logs", isDirectory: true) }
  var gameFiles: URL { assetsDir.appendingPathComponent("game", isDirectory: true) }
  var updateFiles: URL { assetsDir.appendingPathComponent("update", isDirectory: true) }
  var verifiedMarker: URL { gameFiles.appendingPathComponent(".fable2-verified") }
}

func sha1(of url: URL) -> String? {
  guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { return nil }
  return Insecure.SHA1.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func versionString(_ v: UInt32) -> String {
  "\(v >> 28).\((v >> 24) & 0xF).\((v >> 8) & 0xFFFF).\(v & 0xFF)"
}

/// What a game program (default.xex) says about itself: title ID, media ID and
/// version from its execution info header.
struct XexIdentity {
  let titleId: UInt32?
  let mediaId: UInt32?
  let version: UInt32?

  static func read(_ url: URL) -> XexIdentity? {
    guard let handle = try? FileHandle(forReadingFrom: url), let head = try? handle.read(upToCount: 64 * 1024),
          head.count > 24, head.prefix(4) == Data("XEX2".utf8) else {
      return nil
    }
    let bytes = [UInt8](head)
    func u32(_ offset: Int) -> UInt32? {
      guard offset >= 0, offset + 4 <= bytes.count else { return nil }
      return bytes[offset..<offset + 4].reduce(0) { $0 << 8 | UInt32($1) }
    }
    var media: UInt32?, version: UInt32?, title: UInt32?
    let headers = Int(min(u32(20) ?? 0, 64))
    for i in 0..<headers where u32(24 + 8 * i) == 0x0004_0006 {  // execution info
      if let at = u32(28 + 8 * i) {
        media = u32(Int(at))
        version = u32(Int(at) + 4)
        title = u32(Int(at) + 12)
      }
    }
    return XexIdentity(titleId: title, mediaId: media, version: version)
  }

  var isFable2: Bool { titleId == fable2TitleId }
  /// Title update 1 (or later) already built into the program (version 0.0.1.x and up).
  var hasUpdateBuiltIn: Bool { ((version ?? 0) >> 8) & 0xFFFF >= 1 }
  var mediaLabel: String { mediaId.map { String(format: "%08X", $0) } ?? "unknown" }
  var label: String {
    "media ID \(mediaLabel)" + (version.map { ", version \(versionString($0))" } ?? "")
  }
}

/// A title update package (STFS: LIVE/PIRS/CON) for Fable II and the disc it patches.
struct UpdatePackage {
  let url: URL
  let mediaId: UInt32
  let discVersion: UInt32

  static func read(_ url: URL) -> UpdatePackage? {
    guard let handle = try? FileHandle(forReadingFrom: url), let head = try? handle.read(upToCount: 0x400),
          head.count >= 0x364 else {
      return nil
    }
    let bytes = [UInt8](head)
    guard let magic = String(bytes: bytes[0..<4], encoding: .ascii), ["LIVE", "PIRS", "CON "].contains(magic) else {
      return nil
    }
    func u32(_ offset: Int) -> UInt32 { bytes[offset..<offset + 4].reduce(0) { $0 << 8 | UInt32($1) } }
    guard u32(0x344) == 0x000B_0000, u32(0x360) == fable2TitleId else { return nil }  // a title update for Fable II
    return UpdatePackage(url: url, mediaId: u32(0x354), discVersion: u32(0x35C))
  }

  /// Title update packages in a folder (searched a few levels deep) or the file itself.
  static func all(in location: URL) -> [UpdatePackage] {
    let fm = FileManager.default
    var isDirectory: ObjCBool = false
    guard fm.fileExists(atPath: location.path, isDirectory: &isDirectory) else { return [] }
    guard isDirectory.boolValue else { return [read(location)].compactMap { $0 } }
    var found: [UpdatePackage] = []
    let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey]
    guard let walk = fm.enumerator(at: location, includingPropertiesForKeys: keys,
                                   options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { return [] }
    for case let file as URL in walk {
      if walk.level > 4 {
        walk.skipDescendants()
        continue
      }
      let values = try? file.resourceValues(forKeys: Set(keys))
      if values?.isRegularFile == true, (values?.fileSize ?? 0) < 200_000_000, let package = read(file) {
        found.append(package)
      }
    }
    return found
  }
}

// MARK: - Launching

enum Launcher {
  static func isGameRunning() -> Bool {
    let pgrep = Process()
    pgrep.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
    pgrep.arguments = ["-x", "Fable2"]
    pgrep.standardOutput = FileHandle.nullDevice
    pgrep.standardError = FileHandle.nullDevice
    do {
      try pgrep.run()
      pgrep.waitUntilExit()
      return pgrep.terminationStatus == 0
    } catch {
      return false
    }
  }

  static func arguments(paths: GamePaths, mode: DisplayMode, windowSize: String, resolution: String,
                        renderScale: Double, frameCap: Int, vsync: Bool, powerSaving: Bool,
                        titleFps: Bool) -> [String] {
    var args = [paths.assetsDir.path, "--fullscreen=\(mode == .fullscreen)"]
    if mode == .windowed {
      let size = WindowSize.all.first { $0.id == windowSize } ?? WindowSize.all[0]
      args += ["--window_width=\(size.width)", "--window_height=\(size.height)"]
    } else {
      args.append("--eot_resolution=\(resolution)")
    }
    args += [
      "--eot_render_scale=\(renderScale)",
      "--eot_fps_limit=\(frameCap)",
      "--eot_vsync=\(vsync)",
      "--fable2_sleep_waits=\(powerSaving)",
      "--fable2_title_fps=\(titleFps)",
    ]
    if let dump = paths.shaderDumpDir, FileManager.default.fileExists(atPath: dump.path) {
      args.append("--fable2_shader_dump_dir=\(dump.path)")  // keeps shaders the build lacks for the next build
    }
    return args
  }

  /// Starts the game; returns an error message, or nil when it started.
  static func start(paths: GamePaths, arguments: [String]) -> String? {
    let fm = FileManager.default
    guard fm.isExecutableFile(atPath: paths.executable.path) else {
      return "The game isn't built yet: \(paths.executable.path) is missing."
    }
    guard fm.fileExists(atPath: paths.gameFiles.appendingPathComponent("default.xex").path) else {
      return "The game files aren't installed yet: use Install from ISO."
    }
    if isGameRunning() {
      return "Fable 2 is already running. Close it first."
    }
    try? fm.createDirectory(at: paths.logsDir, withIntermediateDirectories: true)
    let output = paths.logsDir.appendingPathComponent("stdio_launcher.txt")
    fm.createFile(atPath: output.path, contents: nil)
    let handle = try? FileHandle(forWritingTo: output)

    let game = Process()
    game.executableURL = paths.executable
    game.arguments = arguments
    game.currentDirectoryURL = paths.gameDir  // the game is always started from its build folder
    game.standardOutput = handle ?? FileHandle.nullDevice
    game.standardError = handle ?? FileHandle.nullDevice
    do {
      try game.run()
    } catch {
      return "Couldn't start the game: \(error.localizedDescription)"
    }
    return nil
  }
}

// MARK: - Installing the game and its title update

@MainActor
final class Installer: ObservableObject {
  enum Phase: Equatable {
    case idle, checking(String), extracting(Double), finishing
  }
  enum UpdateStatus: Equatable {
    case unknown, checking, builtIn, installed, missing, mismatch
  }

  @Published private(set) var phase: Phase = .idle
  @Published private(set) var game: XexIdentity?
  @Published private(set) var update: UpdateStatus = .unknown
  @Published var message: String?
  @Published var messageIsError = false
  @Published var updateLocation: String {
    didSet { UserDefaults.standard.set(updateLocation, forKey: "updateLocation") }
  }

  let paths: GamePaths
  private var process: Process?
  private var cancelled = false

  init(paths: GamePaths) {
    self.paths = paths
    updateLocation = UserDefaults.standard.string(forKey: "updateLocation") ?? ""
    refresh()
  }

  var busy: Bool { phase != .idle }
  var playable: Bool { game != nil && (update == .installed || update == .builtIn) }

  // MARK: Status

  /// Reads the installed program and checks it (from the marker left by the last
  /// check, or with the extractor's fingerprint when the files changed).
  func refresh() {
    let xex = paths.gameFiles.appendingPathComponent("default.xex")
    game = XexIdentity.read(xex)
    guard let game else {
      update = .unknown
      return
    }
    let patch = paths.gameFiles.appendingPathComponent("default.xexp")
    let hasPatch = FileManager.default.fileExists(atPath: patch.path)
    if !hasPatch && !game.hasUpdateBuiltIn {
      update = .missing
      return
    }
    if let marker = try? String(contentsOf: paths.verifiedMarker, encoding: .utf8), marker == markerText() {
      update = hasPatch ? .installed : .builtIn
      return
    }
    update = .checking
    Task {
      let ok = await fingerprintMatches(gameDir: paths.gameFiles)
      if ok { try? markerText().write(to: paths.verifiedMarker, atomically: true, encoding: .utf8) }
      update = ok ? (hasPatch ? .installed : .builtIn) : .mismatch
    }
  }

  private func markerText() -> String {
    let xex = sha1(of: paths.gameFiles.appendingPathComponent("default.xex")) ?? "none"
    let patch = sha1(of: paths.gameFiles.appendingPathComponent("default.xexp")) ?? "none"
    return "image \(paths.expectedImageHash ?? "any")\nxex \(xex)\nxexp \(patch)\n"
  }

  func cancel() {
    cancelled = true
    process?.terminate()
  }

  private func fail(_ text: String) {
    phase = .idle
    message = text
    messageIsError = true
  }

  private func note(_ text: String) {
    message = text
    messageIsError = false
  }

  // MARK: Tools

  /// Runs the extractor; progress lines go to onProgress, other output is returned.
  private func extractor(_ args: [String], onProgress: ((Double) -> Void)? = nil) async
    -> (status: Int32, output: String, errors: String) {
    await withCheckedContinuation { continuation in
      let tool = Process()
      tool.executableURL = paths.extractor
      tool.arguments = args
      tool.currentDirectoryURL = FileManager.default.temporaryDirectory
      let out = Pipe(), err = Pipe()
      tool.standardOutput = out
      tool.standardError = err
      final class Buffers: @unchecked Sendable {
        var pending = "", output = "", errors = ""
      }
      let buffers = Buffers()
      out.fileHandleForReading.readabilityHandler = { handle in
        guard let text = String(data: handle.availableData, encoding: .utf8), !text.isEmpty else { return }
        buffers.pending += text
        while let newline = buffers.pending.firstIndex(of: "\n") {
          let line = String(buffers.pending[..<newline])
          buffers.pending.removeSubrange(...newline)
          let parts = line.split(separator: " ")
          if parts.count == 3, parts[0] == "progress", let done = Double(parts[1]), let total = Double(parts[2]) {
            if total > 0 { onProgress?(done / total) }
          } else {
            buffers.output += line + "\n"
          }
        }
      }
      err.fileHandleForReading.readabilityHandler = { handle in
        buffers.errors += String(data: handle.availableData, encoding: .utf8) ?? ""
      }
      tool.terminationHandler = { finished in
        out.fileHandleForReading.readabilityHandler = nil
        err.fileHandleForReading.readabilityHandler = nil
        let rest = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        continuation.resume(returning: (finished.terminationStatus, buffers.output + buffers.pending + rest,
                                        buffers.errors))
      }
      do {
        try tool.run()
        process = tool
      } catch {
        continuation.resume(returning: (-1, "", error.localizedDescription))
      }
    }
  }

  /// Whether a folder's default.xex (+ default.xexp) loads into exactly the
  /// program this build was recompiled from.
  private func fingerprintMatches(gameDir: URL) async -> Bool {
    guard let expected = paths.expectedImageHash else { return true }
    let (status, output, _) = await extractor(["--image-hash", gameDir.path])
    guard status == 0 else { return false }
    return output.split(separator: "\n").contains { $0 == "image \(expected)" }
  }

  /// The same check for a program and an update kept elsewhere (linked into a
  /// scratch folder).
  private func fingerprintMatches(xex: URL, patch: URL?) async -> Bool {
    let fm = FileManager.default
    let dir = fm.temporaryDirectory.appendingPathComponent("fable2-verify-\(UUID().uuidString)")
    defer { try? fm.removeItem(at: dir) }
    do {
      try fm.createDirectory(at: dir, withIntermediateDirectories: true)
      try fm.createSymbolicLink(at: dir.appendingPathComponent("default.xex"), withDestinationURL: xex)
      if let patch {
        try fm.createSymbolicLink(at: dir.appendingPathComponent("default.xexp"), withDestinationURL: patch)
      }
    } catch {
      return false
    }
    return await fingerprintMatches(gameDir: dir)
  }

  /// Title update packages for a disc, the one made for its media ID first.
  private func updateCandidates(for disc: XexIdentity) -> [UpdatePackage] {
    var locations: [URL] = []
    if !updateLocation.isEmpty { locations.append(URL(fileURLWithPath: updateLocation)) }
    locations.append(paths.assetsDir)  // an update kept next to the game files
    var seen = Set<String>()
    let packages = locations.flatMap { UpdatePackage.all(in: $0) }.filter { seen.insert($0.url.path).inserted }
    return packages.sorted { a, b in
      (a.mediaId == disc.mediaId ? 0 : 1, a.discVersion == disc.version ? 0 : 1) <
        (b.mediaId == disc.mediaId ? 0 : 1, b.discVersion == disc.version ? 0 : 1)
    }
  }

  /// Finds a title update that turns this disc's program into the build's
  /// program; returns its extracted folder (default.xexp, data/...), or nil.
  private func findWorkingUpdate(for disc: XexIdentity, xex: URL) async -> URL? {
    let fm = FileManager.default
    for package in updateCandidates(for: disc).prefix(8) {
      let dir = fm.temporaryDirectory.appendingPathComponent("fable2-update-\(UUID().uuidString)")
      let (status, _, _) = await extractor([package.url.path, dir.path])
      let patch = dir.appendingPathComponent("default.xexp")
      if status == 0, fm.fileExists(atPath: patch.path), await fingerprintMatches(xex: xex, patch: patch) {
        return dir
      }
      try? fm.removeItem(at: dir)
    }
    return nil
  }

  /// Puts an extracted title update in place: default.xexp next to the program
  /// and the whole update in assets/update (the old one goes to the Trash).
  private func placeUpdate(_ updateDir: URL, gameDir: URL) async throws {
    let fm = FileManager.default
    let patch = gameDir.appendingPathComponent("default.xexp")
    try? fm.removeItem(at: patch)
    try fm.copyItem(at: updateDir.appendingPathComponent("default.xexp"), to: patch)
    if fm.fileExists(atPath: paths.updateFiles.path) {
      _ = try await NSWorkspace.shared.recycle([paths.updateFiles])
    }
    try fm.copyItem(at: updateDir, to: paths.updateFiles)
  }

  private func missingUpdateText(_ disc: XexIdentity) -> String {
    let place = updateLocation.isEmpty ? "Choose where your title updates are (Title update → Choose…)"
                                       : "No working one was found in \(updateLocation); choose another location"
    return "This disc (\(disc.label)) needs title update 1 (made for media ID \(disc.mediaLabel)). \(place), "
      + "then try again."
  }

  // MARK: Install the title update for the installed game

  func installUpdate() async {
    guard let disc = game else { return }
    message = nil
    phase = .checking("Looking for title update 1…")
    let xex = paths.gameFiles.appendingPathComponent("default.xex")
    guard let updateDir = await findWorkingUpdate(for: disc, xex: xex) else {
      return fail(missingUpdateText(disc))
    }
    defer { try? FileManager.default.removeItem(at: updateDir) }
    phase = .finishing
    do {
      try await placeUpdate(updateDir, gameDir: paths.gameFiles)
    } catch {
      return fail("Couldn't install the title update: \(error.localizedDescription)")
    }
    try? markerText().write(to: paths.verifiedMarker, atomically: true, encoding: .utf8)
    phase = .idle
    refresh()
    note("Title update 1 installed and checked.")
  }

  // MARK: Install from a disc image

  func install(from iso: URL) async {
    let fm = FileManager.default
    message = nil
    cancelled = false
    guard fm.isExecutableFile(atPath: paths.extractor.path) else {
      return fail("The extractor isn't built: \(paths.extractor.path) is missing.")
    }

    // 1. The disc's program: Fable II, and which version.
    phase = .checking("Checking the disc image…")
    let check = fm.temporaryDirectory.appendingPathComponent("fable2-check-\(UUID().uuidString)")
    defer { try? fm.removeItem(at: check) }
    let discXex = check.appendingPathComponent("default.xex")
    let (checked, _, checkErrors) = await extractor(["--only=default.xex", iso.path, check.path])
    guard checked == 0, let disc = XexIdentity.read(discXex) else {
      return fail("This doesn't look like an Xbox 360 game disc (no game program found). "
                  + checkErrors.prefix(160))
    }
    guard disc.isFable2 else {
      return fail("This disc isn't Fable II (title ID \(disc.titleId.map { String(format: "%08X", $0) } ?? "?")).")
    }

    // 2. With title update 1 (built in, or found), it must give the build's program.
    var updateDir: URL?
    defer { if let updateDir { try? fm.removeItem(at: updateDir) } }
    if disc.hasUpdateBuiltIn {
      phase = .checking("Checking the game program…")
      guard await fingerprintMatches(xex: discXex, patch: nil) else {
        return fail("This disc's program (\(disc.label)) isn't the one this build runs (\(paths.expectedImageLabel)).")
      }
    } else {
      phase = .checking("Looking for title update 1…")
      updateDir = await findWorkingUpdate(for: disc, xex: discXex)
      guard updateDir != nil else { return fail(missingUpdateText(disc)) }
    }

    // 3. Room for the extracted files next to the current ones.
    let isoSize = Int64((try? iso.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
    try? fm.createDirectory(at: paths.assetsDir, withIntermediateDirectories: true)
    if let free = try? paths.assetsDir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
      .volumeAvailableCapacityForImportantUsage, free < isoSize + 1_000_000_000 {
      return fail(String(format: "Not enough free disk space: about %.0f GB are needed, %.0f GB are free.",
                         Double(isoSize + 1_000_000_000) / 1e9, Double(free) / 1e9))
    }

    // 4. Extract into game.new.
    let staging = paths.assetsDir.appendingPathComponent("game.new", isDirectory: true)
    try? fm.removeItem(at: staging)
    phase = .extracting(0)
    let (extracted, _, extractErrors) = await extractor(["--progress", iso.path, staging.path]) { fraction in
      Task { @MainActor in
        if case .extracting = self.phase { self.phase = .extracting(fraction) }
      }
    }
    process = nil
    if cancelled {
      try? fm.removeItem(at: staging)
      phase = .idle
      return note("Cancelled; nothing was changed.")
    }
    guard extracted == 0 else {
      try? fm.removeItem(at: staging)
      return fail("Extracting failed: " + extractErrors.prefix(200))
    }

    // 5. The title update, then the old files go to the Trash and the new ones take their place.
    phase = .finishing
    let hadOld = fm.fileExists(atPath: paths.gameFiles.path)
    do {
      if let updateDir { try await placeUpdate(updateDir, gameDir: staging) }
      if hadOld { _ = try await NSWorkspace.shared.recycle([paths.gameFiles]) }
      try fm.moveItem(at: staging, to: paths.gameFiles)
    } catch {
      return fail("Couldn't put the new game files in place (\(error.localizedDescription)). "
                  + "They are in \(staging.path).")
    }
    try? markerText().write(to: paths.verifiedMarker, atomically: true, encoding: .utf8)
    phase = .idle
    refresh()
    var text = "Installed from \(iso.lastPathComponent) (\(disc.label)"
      + (updateDir != nil ? " + title update 1)." : ").")
    if hadOld { text += " The previous game files are in the Trash." }
    note(text)
  }
}

// MARK: - Window

struct SettingsView: View {
  @AppStorage("displayMode") private var mode: DisplayMode = .windowed
  @AppStorage("windowSize") private var windowSize = WindowSize.all[0].id
  @AppStorage("resolution") private var resolution = "1080p"
  @AppStorage("renderScale") private var renderScale = 1.0
  @AppStorage("frameCap") private var frameCap = 60
  @AppStorage("vsync") private var vsync = true
  @AppStorage("powerSaving") private var powerSaving = true
  @AppStorage("titleFps") private var titleFps = true

  @StateObject private var installer = Installer(paths: GamePaths.load())
  @State private var starting = false

  private var paths: GamePaths { installer.paths }

  var body: some View {
    VStack(spacing: 0) {
      Form {
        gameFilesSection
        Section("Display") {
          Picker("Mode", selection: $mode) {
            ForEach(DisplayMode.allCases) { Text($0.label).tag($0) }
          }
          .pickerStyle(.segmented)
          if mode == .windowed {
            Picker("Window size", selection: $windowSize) {
              ForEach(WindowSize.all) { Text(verbatim: "\($0.width) × \($0.height)").tag($0.id) }
            }
          } else {
            Picker("Resolution", selection: $resolution) {
              ForEach(resolutions, id: \.value) { Text($0.label).tag($0.value) }
            }
          }
          Picker("Render scale", selection: $renderScale) {
            ForEach(renderScales, id: \.value) { Text($0.label).tag($0.value) }
          }
          Text(mode == .windowed
               ? "A window renders at its full pixel size: 2× on a Retina display."
               : "The picture is drawn at this resolution and fitted to the screen.")
            .font(.caption).foregroundStyle(.secondary)
        }
        Section("Performance") {
          Picker("Frame rate cap", selection: $frameCap) {
            ForEach(frameCaps, id: \.value) { Text($0.label).tag($0.value) }
          }
          Toggle("VSync", isOn: $vsync)
          Toggle("Power saving", isOn: $powerSaving)
          Text("Power saving lets idle game threads sleep instead of spinning: less CPU and heat, same frame rate.")
            .font(.caption).foregroundStyle(.secondary)
        }
        Section("Info") {
          Toggle("Show frame rate in the window title", isOn: $titleFps)
          Text("In the game, F8 shows the overlay.")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
      .formStyle(.grouped)

      HStack(alignment: .center) {
        if let message = installer.message {
          Text(message)
            .font(.callout)
            .foregroundStyle(installer.messageIsError ? Color.red : Color.secondary)
            .lineLimit(5)
            .fixedSize(horizontal: false, vertical: true)
        }
        Spacer()
        Button("Play", action: play)
          .keyboardShortcut(.defaultAction)
          .controlSize(.large)
          .disabled(starting || installer.busy || !installer.playable)
      }
      .padding([.horizontal, .bottom], 20)
      .padding(.top, 4)
    }
    .frame(width: 480, height: 800)
    .onAppear {
      // Command line (open "Fable 2.app" --args ...): --play starts at once with the saved settings;
      // --update-location <path> and --install-iso <path> do what the buttons do, without dialogs.
      let args = CommandLine.arguments
      func value(_ name: String) -> String? {
        guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
        return args[i + 1]
      }
      if let location = value("--update-location") { installer.updateLocation = location }
      if let iso = value("--install-iso") {
        Task { await installer.install(from: URL(fileURLWithPath: iso)) }
      } else if value("--update-location") != nil && installer.update == .missing {
        Task { await installer.installUpdate() }
      } else if args.contains("--play") {
        play()
      }
    }
  }

  @ViewBuilder private var gameFilesSection: some View {
    Section("Game files") {
      HStack {
        if let game = installer.game {
          Label("Fable II · media ID \(game.mediaLabel)" + (game.version.map { " · \(versionString($0))" } ?? ""),
                systemImage: "opticaldisc")
            .lineLimit(1)
        } else {
          Label("Not installed", systemImage: "exclamationmark.circle").foregroundStyle(.orange)
        }
        Spacer()
        Button("Install from ISO…", action: chooseDiscImage)
          .disabled(installer.busy)
      }
      HStack {
        updateStatusLabel
        Spacer()
        if installer.game != nil && (installer.update == .missing || installer.update == .mismatch) {
          Button("Install update") { Task { await installer.installUpdate() } }
            .disabled(installer.busy)
        }
        Button("Choose…", action: chooseUpdateLocation)
          .disabled(installer.busy)
      }
      Text(installer.updateLocation.isEmpty
           ? "Title update 1 isn't on the disc. Choose the folder (or file) with your title updates; the one for "
             + "your disc is picked by its media ID."
           : "Title updates are looked for in \(installer.updateLocation).")
        .font(.caption).foregroundStyle(.secondary)
        .lineLimit(3)
      switch installer.phase {
      case .idle:
        EmptyView()
      case .checking(let what):
        ProgressView(what).progressViewStyle(.linear)
      case .extracting(let fraction):
        HStack {
          ProgressView(value: fraction) {
            Text("Extracting… \(Int(fraction * 100))%")
          }
          Button("Cancel") { installer.cancel() }
        }
      case .finishing:
        ProgressView("Finishing…").progressViewStyle(.linear)
      }
    }
  }

  @ViewBuilder private var updateStatusLabel: some View {
    switch installer.update {
    case .unknown:
      Label("Title update 1", systemImage: "circle.dashed").foregroundStyle(.secondary)
    case .checking:
      Label("Title update 1: checking…", systemImage: "hourglass").foregroundStyle(.secondary)
    case .installed:
      Label("Title update 1 installed", systemImage: "checkmark.circle").foregroundStyle(.green)
    case .builtIn:
      Label("Title update 1 built into the game", systemImage: "checkmark.circle").foregroundStyle(.green)
    case .missing:
      Label("Title update 1 missing", systemImage: "exclamationmark.circle").foregroundStyle(.orange)
    case .mismatch:
      Label("Doesn't match this build", systemImage: "xmark.octagon").foregroundStyle(.red)
    }
  }

  private func chooseDiscImage() {
    let panel = NSOpenPanel()
    panel.title = "Choose a Fable II disc image"
    panel.message = "Choose the game's .iso file. The game files are extracted from it."
    panel.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .diskImage]
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    guard panel.runModal() == .OK, let iso = panel.url else { return }
    if installer.game != nil {
      let alert = NSAlert()
      alert.messageText = "Replace the installed game files?"
      alert.informativeText = "The disc is checked first. The current game files are moved to the Trash once the "
        + "new ones are extracted. Your saves are kept."
      alert.addButton(withTitle: "Replace")
      alert.addButton(withTitle: "Cancel")
      guard alert.runModal() == .alertFirstButtonReturn else { return }
    }
    Task { await installer.install(from: iso) }
  }

  private func chooseUpdateLocation() {
    let panel = NSOpenPanel()
    panel.title = "Where are your title updates?"
    panel.message = "Choose a folder with title update packages (for example FableII_TU1), or one update file."
    panel.canChooseDirectories = true
    panel.canChooseFiles = true
    panel.allowsMultipleSelection = false
    guard panel.runModal() == .OK, let url = panel.url else { return }
    let found = UpdatePackage.all(in: url)
    installer.updateLocation = url.path
    if found.isEmpty {
      installer.message = "No Fable II title update was found in \(url.lastPathComponent)."
      installer.messageIsError = true
    } else {
      let medias = Set(found.map { String(format: "%08X", $0.mediaId) }).sorted().joined(separator: ", ")
      installer.message = "Found \(found.count) title update\(found.count == 1 ? "" : "s") (media IDs \(medias))."
      installer.messageIsError = false
      if installer.update == .missing || installer.update == .mismatch {
        Task { await installer.installUpdate() }
      }
    }
  }

  private func play() {
    starting = true
    let args = Launcher.arguments(paths: paths, mode: mode, windowSize: windowSize, resolution: resolution,
                                  renderScale: renderScale, frameCap: frameCap, vsync: vsync,
                                  powerSaving: powerSaving, titleFps: titleFps)
    if let error = Launcher.start(paths: paths, arguments: args) {
      installer.message = error
      installer.messageIsError = true
      starting = false
      return
    }
    // The game runs on its own; the launcher's job is done.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NSApp.terminate(nil) }
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct Fable2LauncherApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

  var body: some Scene {
    Window("Fable 2", id: "settings") {
      SettingsView()
    }
    .windowResizability(.contentSize)
  }
}

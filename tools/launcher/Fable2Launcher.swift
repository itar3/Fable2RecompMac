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
// The release app (tools/release/make_release.sh) carries no game code: its
// Info.plist has Fable2PayloadVersion, and its Resources a payload (this
// project's source, the ReXGlue SDK, CMake, Ninja and fable2-extract). On the
// first start of each version the payload is unpacked into
// ~/Library/Application Support/Fable2RecompMac, and after the game files are
// installed, Build generates the game's code from the player's own copy and
// compiles it there (needs Apple's Command Line Tools); --build does the same.
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
let frameCaps: [(value: Int, label: String)] = [(0, "Off"), (30, "30 fps"), (40, "40 fps"), (60, "60 fps")]

let fable2TitleId: UInt32 = 0x4D53_07F1

// MARK: - Paths

struct GamePaths {
  let gameDir: URL
  let assetsDir: URL
  let shaderDumpDir: URL?
  let expectedImageHash: String?
  let expectedImageLabel: String
  /// Release app: where the payload is unpacked and the game is built, and the payload's version.
  var workspace: URL? = nil
  var payloadVersion: String? = nil

  static func load() -> GamePaths {
    let info = Bundle.main.infoDictionary ?? [:]
    let bundleParent = Bundle.main.bundleURL.deletingLastPathComponent()
    func string(_ key: String) -> String? {
      guard let value = info[key] as? String, !value.isEmpty, !value.hasPrefix("@") else { return nil }
      return value
    }
    func dir(_ key: String) -> URL? { string(key).map { URL(fileURLWithPath: $0, isDirectory: true) } }
    let hash = string("Fable2GameImageHash")?.lowercased()
    let label = string("Fable2GameImageLabel") ?? "the program this build was made from"
    if let version = string("Fable2PayloadVersion") {
      let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      let workspace = support.appendingPathComponent("Fable2RecompMac", isDirectory: true)
      return GamePaths(gameDir: workspace.appendingPathComponent("build", isDirectory: true),
                       assetsDir: workspace.appendingPathComponent("assets", isDirectory: true),
                       shaderDumpDir: nil, expectedImageHash: hash, expectedImageLabel: label,
                       workspace: workspace, payloadVersion: version)
    }
    let game = dir("Fable2GameDir") ?? bundleParent
    return GamePaths(gameDir: game, assetsDir: dir("Fable2AssetsDir") ?? game.appendingPathComponent("assets"),
                     shaderDumpDir: dir("Fable2ShaderDumpDir"),
                     expectedImageHash: hash, expectedImageLabel: label)
  }

  var executable: URL { gameDir.appendingPathComponent("Fable2") }
  var extractor: URL {
    (workspace?.appendingPathComponent("tools", isDirectory: true) ?? gameDir).appendingPathComponent("fable2-extract")
  }
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

// MARK: - Running tools

/// The tool the launcher is running (extractor, build), stopped when the launcher quits.
nonisolated(unsafe) var currentTool: Process?

/// Runs a command-line tool. Each line of its standard output goes to `line`;
/// the lines it doesn't take (returns false) are returned with the standard
/// error. Everything it prints also goes to `log`.
func runTool(_ executable: URL, _ args: [String], directory: URL, environment: [String: String]? = nil,
             log: FileHandle? = nil, line: @escaping @Sendable (String) -> Bool = { _ in false }) async
  -> (status: Int32, output: String, errors: String) {
  await withCheckedContinuation { continuation in
    let tool = Process()
    tool.executableURL = executable
    tool.arguments = args
    tool.currentDirectoryURL = directory
    if let environment { tool.environment = environment }
    let out = Pipe(), err = Pipe()
    tool.standardOutput = out
    tool.standardError = err
    final class Buffers: @unchecked Sendable {
      let lock = NSLock()
      var pending = "", output = "", errors = ""
      func take(_ data: Data, isError: Bool, final: Bool = false) {
        lock.lock()
        defer { lock.unlock() }
        let text = String(decoding: data, as: UTF8.self)
        if isError {
          errors += text
          return
        }
        pending += text
        while let newline = pending.firstIndex(of: "\n") {
          let text = String(pending[..<newline])
          pending.removeSubrange(...newline)
          if !lineHandler(text) { output += text + "\n" }
        }
        if final, !pending.isEmpty, !lineHandler(pending) { output += pending }
      }
      var lineHandler: (String) -> Bool = { _ in false }
    }
    let buffers = Buffers()
    buffers.lineHandler = line
    out.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      log?.write(data)
      buffers.take(data, isError: false)
    }
    err.fileHandleForReading.readabilityHandler = { handle in
      let data = handle.availableData
      guard !data.isEmpty else { return }
      log?.write(data)
      buffers.take(data, isError: true)
    }
    tool.terminationHandler = { finished in
      out.fileHandleForReading.readabilityHandler = nil
      err.fileHandleForReading.readabilityHandler = nil
      let rest = out.fileHandleForReading.readDataToEndOfFile()
      let restErrors = err.fileHandleForReading.readDataToEndOfFile()
      log?.write(rest)
      log?.write(restErrors)
      buffers.take(restErrors, isError: true)
      buffers.take(rest, isError: false, final: true)
      buffers.lock.lock()
      let result = (finished.terminationStatus, buffers.output, buffers.errors)
      buffers.lock.unlock()
      continuation.resume(returning: result)
    }
    do {
      try tool.run()
      currentTool = tool
    } catch {
      continuation.resume(returning: (-1, "", error.localizedDescription))
    }
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
      return paths.workspace != nil ? "The game isn't built yet: click Build."
                                    : "The game isn't built yet: \(paths.executable.path) is missing."
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
    /// Building the game (release app): what, and how far when known.
    case building(String, Double?)
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

  /// Release app: whether the game is built from this app's payload (always true otherwise).
  @Published private(set) var built = true

  let paths: GamePaths
  private var cancelled = false
  /// The running check of the installed program, if any.
  private var check: Task<Void, Never>?

  init(paths: GamePaths) {
    self.paths = paths
    updateLocation = UserDefaults.standard.string(forKey: "updateLocation") ?? ""
    refresh()
    refreshBuilt()
  }

  var busy: Bool { phase != .idle }
  var gameReady: Bool { game != nil && (update == .installed || update == .builtIn) }
  var playable: Bool { gameReady && built }
  var needsBuild: Bool { gameReady && !built }

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
    check = Task {
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
    currentTool?.terminate()
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
  private func extractor(_ args: [String], onProgress: (@Sendable (Double) -> Void)? = nil) async
    -> (status: Int32, output: String, errors: String) {
    await runTool(paths.extractor, args, directory: FileManager.default.temporaryDirectory) { line in
      let parts = line.split(separator: " ")
      guard parts.count == 3, parts[0] == "progress", let done = Double(parts[1]), let total = Double(parts[2]) else {
        return false
      }
      if total > 0 { onProgress?(done / total) }
      return true
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
    if needsBuild { await build() }
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
    if needsBuild { await build() }
  }
}

// MARK: - Release app: unpacking the payload and building the game

extension Installer {
  private var buildStamp: URL { paths.gameDir.appendingPathComponent(".fable2-built") }

  func refreshBuilt() {
    guard paths.workspace != nil else { return }
    built = (try? String(contentsOf: buildStamp, encoding: .utf8)) == paths.payloadVersion
      && FileManager.default.isExecutableFile(atPath: paths.executable.path)
  }

  /// First start of this app version: its payload (source, SDK, CMake, Ninja,
  /// fable2-extract) replaces the workspace's. The game files, the build folder
  /// and the saves (~/.local/share/Fable2) are kept. Returns false on failure.
  func unpackPayload() async -> Bool {
    guard let workspace = paths.workspace, let version = paths.payloadVersion else { return true }
    let fm = FileManager.default
    let stamp = workspace.appendingPathComponent(".payload-version")
    if (try? String(contentsOf: stamp, encoding: .utf8)) == version { return true }
    guard let payload = Bundle.main.url(forResource: "payload", withExtension: "tar.xz") else {
      fail("This app is incomplete (its payload is missing). Download it again.")
      return false
    }
    phase = .checking("First start: unpacking the build tools…")
    let staging = workspace.appendingPathComponent(".unpack", isDirectory: true)
    try? fm.removeItem(at: staging)
    do {
      try fm.createDirectory(at: staging, withIntermediateDirectories: true)
    } catch {
      fail("Couldn't create \(workspace.path): \(error.localizedDescription)")
      return false
    }
    let (status, _, errors) = await runTool(URL(fileURLWithPath: "/usr/bin/tar"), ["-xf", payload.path, "-C", staging.path],
                                            directory: staging)
    guard status == 0 else {
      try? fm.removeItem(at: staging)
      fail("Couldn't unpack the build tools: " + errors.prefix(200))
      return false
    }
    // Tools unpacked by a downloaded app may carry its quarantine flag, which would stop them from running.
    _ = await runTool(URL(fileURLWithPath: "/usr/bin/xattr"), ["-dr", "com.apple.quarantine", staging.path],
                      directory: staging)
    do {
      for name in ["source", "sdk", "tools"] {
        let target = workspace.appendingPathComponent(name, isDirectory: true)
        try? fm.removeItem(at: target)
        try fm.moveItem(at: staging.appendingPathComponent(name), to: target)
      }
      try fm.removeItem(at: staging)
      try fm.createDirectory(at: paths.assetsDir, withIntermediateDirectories: true)
      // Code generation reads assets/game/default.xex next to the manifest.
      try fm.createSymbolicLink(atPath: workspace.appendingPathComponent("source/assets").path,
                                withDestinationPath: "../assets")
      try? fm.removeItem(at: buildStamp)
      try version.write(to: stamp, atomically: true, encoding: .utf8)
    } catch {
      fail("Couldn't set up \(workspace.path): \(error.localizedDescription)")
      return false
    }
    phase = .idle
    refresh()  // the game files' check needs the extractor just unpacked
    refreshBuilt()
    return true
  }

  /// Apple's Command Line Tools (clang, the macOS SDK, git), or Xcode.
  private func commandLineToolsInstalled() async -> Bool {
    let xcrun = URL(fileURLWithPath: "/usr/bin/xcrun")
    let tmp = FileManager.default.temporaryDirectory
    let (clang, _, _) = await runTool(xcrun, ["--find", "clang"], directory: tmp)
    let (sdk, _, _) = await runTool(xcrun, ["--show-sdk-path"], directory: tmp)
    let (git, _, _) = await runTool(xcrun, ["--find", "git"], directory: tmp)
    return clang == 0 && sdk == 0 && git == 0
  }

  /// Generates the game's code from the installed game files and compiles it
  /// in the workspace. Stopping keeps what is done: the next build goes on from there.
  func build() async {
    guard let workspace = paths.workspace, let version = paths.payloadVersion else { return }
    let fm = FileManager.default
    message = nil
    cancelled = false
    guard await unpackPayload() else { return }
    await check?.value
    guard gameReady else { return fail("Install the game files (and title update 1) first.") }

    phase = .checking("Checking the Command Line Tools…")
    guard await commandLineToolsInstalled() else {
      phase = .idle
      _ = try? Process.run(URL(fileURLWithPath: "/usr/bin/xcode-select"), arguments: ["--install"])
      return fail("Building the game needs Apple's Command Line Tools. Install them in the window that opened "
                  + "(it takes a few minutes), then click Build.")
    }

    let source = workspace.appendingPathComponent("source", isDirectory: true)
    let sdk = workspace.appendingPathComponent("sdk", isDirectory: true)
    let tools = workspace.appendingPathComponent("tools", isDirectory: true)
    let logURL = workspace.appendingPathComponent("build.log")
    fm.createFile(atPath: logURL.path, contents: nil)
    let log = try? FileHandle(forWritingTo: logURL)
    defer { try? log?.close() }
    var environment = ProcessInfo.processInfo.environment
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    // The patches for thirdparty/ are applied with 'git apply', which must not find a repository above the workspace.
    environment["GIT_CEILING_DIRECTORIES"] = workspace.deletingLastPathComponent().path
    environment["NINJA_STATUS"] = "[%f/%t] "
    func stopped() {
      phase = .idle
      note("Building stopped. Click Build to go on from where it stopped.")
    }
    func failed(_ step: String) {
      fail("\(step) failed. The details are in \(logURL.path).")
    }

    // 1. The game's code, from the player's copy (generated/; done once per payload).
    let generatedMarker = source.appendingPathComponent("generated/.fable2-generated")
    if !fm.fileExists(atPath: generatedMarker.path) {
      phase = .building("Generating the game's code…", nil)
      let (status, _, _) = await runTool(sdk.appendingPathComponent("bin/rexglue"), ["codegen", "Fable2_manifest.toml"],
                                         directory: source, environment: environment, log: log) { _ in true }
      if cancelled { return stopped() }
      guard status == 0 else { return failed("Generating the game's code") }
      fm.createFile(atPath: generatedMarker.path, contents: Data(version.utf8))
    }

    // 2. Configure (once; Ninja reconfigures by itself when needed).
    if !fm.fileExists(atPath: paths.gameDir.appendingPathComponent("build.ninja").path) {
      phase = .building("Preparing the build…", nil)
      let (status, _, _) = await runTool(
        tools.appendingPathComponent("cmake/bin/cmake"),
        ["-S", source.path, "-B", paths.gameDir.path, "-G", "Ninja",
         "-DCMAKE_MAKE_PROGRAM=\(tools.appendingPathComponent("ninja").path)",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
         "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_PREFIX_PATH=\(sdk.path)",
         "-DFABLE2_NATIVE_RENDERER=ON", "-DFABLE2_LAUNCHER=OFF"],
        directory: workspace, environment: environment, log: log) { _ in true }
      if cancelled { return stopped() }
      guard status == 0 else {
        try? fm.removeItem(at: paths.gameDir.appendingPathComponent("build.ninja"))
        return failed("Preparing the build")
      }
    }

    // 3. Compile, with fewer jobs on Macs with less memory (the generated files are large).
    let info = ProcessInfo.processInfo
    let jobs = max(2, min(info.activeProcessorCount, Int(info.physicalMemory >> 30) * 2 / 3))
    phase = .building("Compiling…", 0)
    let (status, _, _) = await runTool(tools.appendingPathComponent("ninja"),
                                       ["-C", paths.gameDir.path, "-j", "\(jobs)", "Fable2"],
                                       directory: workspace, environment: environment, log: log) { line in
      // "[done/total] what": the progress.
      if line.hasPrefix("["), let close = line.firstIndex(of: "]") {
        let counts = line[line.index(after: line.startIndex)..<close].split(separator: "/")
        if counts.count == 2, let done = Double(counts[0]), let total = Double(counts[1]), total > 0 {
          Task { @MainActor in
            if case .building = self.phase {
              self.phase = .building("Compiling… \(Int(done)) of \(Int(total))", done / total)
            }
          }
        }
      }
      return true
    }
    if cancelled { return stopped() }
    guard status == 0 else { return failed("Compiling") }
    try? version.write(to: buildStamp, atomically: true, encoding: .utf8)
    phase = .idle
    refreshBuilt()
    note("The game is built. Click Play.")
  }
}

// MARK: - Keyboard and mouse

/// One controller input the keyboard and mouse can drive: the game's
/// --keybind_<id> option (ReXGlue's keyboard/mouse driver), its label here and
/// its default keys. Keys are ReXGlue key names by physical position (as on a
/// US keyboard), comma-separated alternatives, with optional Shift+/Ctrl+/Alt+.
struct KeyBinding: Identifiable {
  let id: String
  let label: String
  let defaults: String
}

let keyBindingGroups: [(title: String, bindings: [KeyBinding])] = [
  ("Movement and camera", [
    KeyBinding(id: "lstick_up", label: "Move forward", defaults: "W"),
    KeyBinding(id: "lstick_down", label: "Move back", defaults: "S"),
    KeyBinding(id: "lstick_left", label: "Move left", defaults: "A"),
    KeyBinding(id: "lstick_right", label: "Move right", defaults: "D"),
    KeyBinding(id: "lstick_press", label: "Left stick click", defaults: "V"),
    KeyBinding(id: "rstick_up", label: "Camera up", defaults: "I"),
    KeyBinding(id: "rstick_down", label: "Camera down", defaults: "K"),
    KeyBinding(id: "rstick_left", label: "Camera left", defaults: "J"),
    KeyBinding(id: "rstick_right", label: "Camera right", defaults: "L"),
    KeyBinding(id: "rstick_press", label: "Right stick click", defaults: "MMB,G"),
  ]),
  ("Controller buttons", [
    KeyBinding(id: "a", label: "A (interact, confirm)", defaults: "Space,E"),
    KeyBinding(id: "b", label: "B (magic, back)", defaults: "Q,Backspace"),
    KeyBinding(id: "x", label: "X (melee)", defaults: "LMB,F"),
    KeyBinding(id: "y", label: "Y (ranged)", defaults: "RMB,R"),
    KeyBinding(id: "left_shoulder", label: "LB", defaults: "1"),
    KeyBinding(id: "right_shoulder", label: "RB", defaults: "2"),
    KeyBinding(id: "left_trigger", label: "LT", defaults: "Z"),
    KeyBinding(id: "right_trigger", label: "RT", defaults: "C"),
    KeyBinding(id: "dpad_up", label: "D-pad up", defaults: "Up"),
    KeyBinding(id: "dpad_down", label: "D-pad down", defaults: "Down"),
    KeyBinding(id: "dpad_left", label: "D-pad left", defaults: "Left"),
    KeyBinding(id: "dpad_right", label: "D-pad right", defaults: "Right"),
    KeyBinding(id: "back", label: "Back", defaults: "Tab,M"),
    KeyBinding(id: "start", label: "Start (pause menu)", defaults: "Return,Escape"),
  ]),
]

enum KeyBindings {
  static var all: [KeyBinding] { keyBindingGroups.flatMap(\.bindings) }

  static func keys(_ binding: KeyBinding) -> [String] {
    let value = UserDefaults.standard.string(forKey: "keybind." + binding.id) ?? binding.defaults
    return value.split(separator: ",").map(String.init)
  }

  static func set(_ binding: KeyBinding, _ keys: [String]) {
    let value = keys.joined(separator: ",")
    if value == binding.defaults {
      UserDefaults.standard.removeObject(forKey: "keybind." + binding.id)
    } else {
      UserDefaults.standard.set(value, forKey: "keybind." + binding.id)
    }
  }

  static func restoreDefaults() {
    for binding in all { UserDefaults.standard.removeObject(forKey: "keybind." + binding.id) }
  }

  /// The game's options for every binding ("None" for an input with no key).
  static func arguments() -> [String] {
    all.map { binding in
      let value = keys(binding).joined(separator: ",")
      return "--keybind_\(binding.id)=\(value.isEmpty ? "None" : value)"
    }
  }

  /// ReXGlue's name of a key by its position (macOS key code, US layout).
  static let names: [UInt16: String] = [
    0: "A", 1: "S", 2: "D", 3: "F", 4: "H", 5: "G", 6: "Z", 7: "X", 8: "C", 9: "V", 11: "B", 12: "Q", 13: "W",
    14: "E", 15: "R", 16: "Y", 17: "T", 18: "1", 19: "2", 20: "3", 21: "4", 22: "6", 23: "5", 24: "Plus", 25: "9",
    26: "7", 27: "Minus", 28: "8", 29: "0", 30: "RBracket", 31: "O", 32: "U", 33: "LBracket", 34: "I", 35: "P",
    36: "Return", 37: "L", 38: "J", 39: "Quote", 40: "K", 41: "Semicolon", 42: "Backslash", 43: "Comma",
    44: "Slash", 45: "N", 46: "M", 47: "Period", 48: "Tab", 49: "Space", 50: "Backtick", 51: "Backspace",
    53: "Escape", 67: "NumpadStar", 69: "NumpadPlus", 75: "NumpadSlash", 76: "NumpadEnter", 78: "NumpadMinus",
    82: "Numpad0", 83: "Numpad1", 84: "Numpad2", 85: "Numpad3", 86: "Numpad4", 87: "Numpad5", 88: "Numpad6",
    89: "Numpad7", 91: "Numpad8", 92: "Numpad9", 122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
    98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12", 105: "F13", 107: "F14", 113: "F15",
    106: "F16", 64: "F17", 79: "F18", 80: "F19", 90: "F20", 114: "Insert", 115: "Home", 116: "PageUp",
    117: "Delete", 119: "End", 121: "PageDown", 123: "Left", 124: "Right", 125: "Down", 126: "Up",
  ]

  /// How a key name is shown (Mac key labels).
  static func label(_ name: String) -> String {
    var rest = Substring(name), prefix = ""
    for (modifier, symbol) in [("Shift+", "⇧"), ("Ctrl+", "⌃"), ("Alt+", "⌥")] where rest.hasPrefix(modifier) {
      prefix += symbol
      rest = rest.dropFirst(modifier.count)
    }
    let labels = ["LMB": "Left click", "RMB": "Right click", "MMB": "Middle click", "Backspace": "Delete",
                  "Delete": "Fwd Delete", "Escape": "Esc", "Up": "↑", "Down": "↓", "Left": "←", "Right": "→",
                  "Plus": "=", "Minus": "-", "LBracket": "[", "RBracket": "]", "Semicolon": ";", "Quote": "'",
                  "Backslash": "\\", "Comma": ",", "Period": ".", "Slash": "/", "Backtick": "`"]
    return prefix + (labels[String(rest)] ?? String(rest))
  }
}

/// The key bindings window: each input with two key slots. Click a slot, then
/// press a key (with Shift, Control or Option if wanted) or click a mouse
/// button; Esc cancels. A key moves away from the input that had it.
struct KeyBindingsView: View {
  @Environment(\.dismiss) private var dismiss
  @State private var keys: [String: [String]] = [:]
  @State private var capturing: (id: String, slot: Int)?
  @State private var monitor: Any?
  @State private var note: String?

  var body: some View {
    VStack(spacing: 0) {
      Form {
        ForEach(keyBindingGroups, id: \.title) { group in
          Section(group.title) {
            ForEach(group.bindings) { binding in
              HStack {
                Text(binding.label)
                Spacer()
                slot(binding, 0)
                slot(binding, 1)
              }
            }
          }
        }
      }
      .formStyle(.grouped)
      HStack {
        Text(note ?? "Click a slot, then press a key or click a mouse button (Esc cancels). Right-click a slot to "
             + "clear it. A controller works alongside.")
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
        Spacer()
        Button("Restore defaults") {
          KeyBindings.restoreDefaults()
          load()
          note = "Default keys restored."
        }
        Button("Done") { dismiss() }
          .keyboardShortcut(.defaultAction)
      }
      .padding(16)
    }
    .frame(width: 520, height: 680)
    .onAppear(perform: load)
    .onDisappear(perform: stopCapture)
  }

  private func load() {
    keys = Dictionary(uniqueKeysWithValues: KeyBindings.all.map { ($0.id, KeyBindings.keys($0)) })
  }

  @ViewBuilder private func slot(_ binding: KeyBinding, _ index: Int) -> some View {
    let current = keys[binding.id] ?? []
    let active = capturing?.id == binding.id && capturing?.slot == index
    Button {
      startCapture(binding.id, index)
    } label: {
      Text(active ? "Press…" : (index < current.count ? KeyBindings.label(current[index]) : " "))
        .frame(width: 92)
    }
    .buttonStyle(.bordered)
    .tint(active ? .accentColor : nil)
    .contextMenu {
      if index < current.count {
        Button("Clear") { assign(nil, to: binding.id, slot: index) }
      }
    }
  }

  private func startCapture(_ id: String, _ slot: Int) {
    stopCapture()
    capturing = (id, slot)
    note = nil
    // Starts after this click, so the click that opened the slot isn't taken as a binding.
    DispatchQueue.main.async {
      monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .leftMouseDown, .rightMouseDown,
                                                             .otherMouseDown]) { event in
        capture(event)
        return nil
      }
    }
  }

  private func stopCapture() {
    if let monitor { NSEvent.removeMonitor(monitor) }
    monitor = nil
    capturing = nil
  }

  private func capture(_ event: NSEvent) {
    guard let (id, slot) = capturing else { return }
    var name: String?
    switch event.type {
    case .leftMouseDown: name = "LMB"
    case .rightMouseDown: name = "RMB"
    case .otherMouseDown:
      name = event.buttonNumber == 2 ? "MMB" : nil
      if name == nil { note = "Only the left, right and middle mouse buttons can be used." }
    case .keyDown:
      if event.keyCode == 53 && event.modifierFlags.intersection([.shift, .control, .option]).isEmpty {
        return stopCapture()  // Esc cancels
      }
      if event.modifierFlags.contains(.command) {
        note = "Command shortcuts can't be used."
      } else if let key = KeyBindings.names[event.keyCode] {
        var prefix = ""
        if event.modifierFlags.contains(.shift) { prefix += "Shift+" }
        if event.modifierFlags.contains(.control) { prefix += "Ctrl+" }
        if event.modifierFlags.contains(.option) { prefix += "Alt+" }
        name = prefix + key
      } else {
        note = "That key can't be used."
      }
    default: break
    }
    stopCapture()
    if let name { assign(name, to: id, slot: slot) }
  }

  /// Puts a key in a slot (nil clears it); the key leaves any other input that had it.
  private func assign(_ name: String?, to id: String, slot: Int) {
    if let name {
      for binding in KeyBindings.all where binding.id != id && (keys[binding.id] ?? []).contains(name) {
        keys[binding.id]?.removeAll { $0 == name }
        note = "\(KeyBindings.label(name)) moved from \(binding.label)."
      }
    }
    var current = keys[id] ?? []
    if let name {
      current.removeAll { $0 == name }
      if slot < current.count { current[slot] = name } else { current.append(name) }
    } else if slot < current.count {
      current.remove(at: slot)
    }
    keys[id] = current
    for binding in KeyBindings.all { KeyBindings.set(binding, keys[binding.id] ?? []) }
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
  @AppStorage("keyboardMouse") private var keyboardMouse = true
  @AppStorage("mouseLook") private var mouseLook = true
  @AppStorage("mouseSensitivity") private var mouseSensitivity = 1.0
  @State private var showKeyBindings = false

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
          if frameCap == 40 {
            Text("40 fps is smooth on 120 Hz screens (ProMotion) and does about a third less GPU work than 60: a cooler, quieter Mac. On 60 Hz screens it judders; use 30 or 60 there.")
              .font(.caption).foregroundStyle(.secondary)
          }
          Toggle("VSync", isOn: $vsync)
          Toggle("Power saving", isOn: $powerSaving)
          Text("Power saving lets idle game threads sleep instead of spinning: less CPU and heat, same frame rate.")
            .font(.caption).foregroundStyle(.secondary)
        }
        Section("Controls") {
          Toggle("Keyboard and mouse", isOn: $keyboardMouse)
          if keyboardMouse {
            Toggle("Mouse moves the camera", isOn: $mouseLook)
            if mouseLook {
              HStack {
                Text("Mouse sensitivity")
                Slider(value: $mouseSensitivity, in: 0.2...3.0)
                Text(String(format: "%.1f", mouseSensitivity)).monospacedDigit().frame(width: 30)
              }
            }
            HStack {
              Text("WASD moves, the mouse buttons attack (melee, ranged), Space or E interacts.")
                .font(.caption).foregroundStyle(.secondary)
              Spacer()
              Button("Key bindings…") { showKeyBindings = true }
            }
          }
          Text("A controller always works too. Switch apps (⌘-Tab) to get the mouse pointer back.")
            .font(.caption).foregroundStyle(.secondary)
        }
        .sheet(isPresented: $showKeyBindings) { KeyBindingsView() }
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
      // Release app: --build builds the game.
      Task {
        guard await installer.unpackPayload() else { return }
        if let location = value("--update-location") { installer.updateLocation = location }
        if let iso = value("--install-iso") {
          await installer.install(from: URL(fileURLWithPath: iso))
        } else if value("--update-location") != nil && installer.update == .missing {
          await installer.installUpdate()
        } else if args.contains("--build") {
          await installer.build()
        } else if args.contains("--play") {
          play()
        }
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
      if paths.workspace != nil {
        HStack {
          if installer.built {
            Label("Game built on this Mac", systemImage: "hammer").foregroundStyle(.green)
          } else {
            Label("Game not built yet", systemImage: "hammer").foregroundStyle(.orange)
          }
          Spacer()
          if !installer.built {
            Button("Build") { Task { await installer.build() } }
              .disabled(installer.busy || !installer.gameReady)
          }
        }
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
      case .building(let what, let fraction):
        HStack {
          if let fraction {
            ProgressView(value: fraction) { Text(what) }
          } else {
            ProgressView(what).progressViewStyle(.linear)
          }
          Button("Stop") { installer.cancel() }
        }
      }
      if paths.workspace != nil && !installer.built {
        Text("The game's code is generated from your copy of the game and compiled on this Mac, once: 2 to 15 "
             + "minutes, about 1 GB. It needs Apple's Command Line Tools (offered when missing).")
          .font(.caption).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
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

  /// Keyboard and mouse: ReXGlue's driver, which the game merges with a controller.
  private func controlArguments() -> [String] {
    guard keyboardMouse else { return ["--mnk_mode=false"] }
    return ["--mnk_mode=true", "--mnk_mouse=\(mouseLook)", String(format: "--mnk_sensitivity=%.2f", mouseSensitivity)]
      + KeyBindings.arguments()
  }

  private func play() {
    starting = true
    let args = Launcher.arguments(paths: paths, mode: mode, windowSize: windowSize, resolution: resolution,
                                  renderScale: renderScale, frameCap: frameCap, vsync: vsync,
                                  powerSaving: powerSaving, titleFps: titleFps)
      + controlArguments()
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
  func applicationWillTerminate(_ notification: Notification) {
    if let tool = currentTool, tool.isRunning { tool.terminate() }  // a build goes on from there next time
  }
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

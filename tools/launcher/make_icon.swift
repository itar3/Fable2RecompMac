// Draws the launcher's icon (an original design: a gold "II" on a dusk-blue
// rounded square) as a 1024 x 1024 PNG. tools/launcher/make_icon.sh turns it
// into AppIcon.icns (committed).  Usage: swift make_icon.swift <out.png>
import AppKit
import CoreText

let size = 1024.0
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
let space = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: Int(size), height: Int(size), bitsPerComponent: 8, bytesPerRow: 0,
                    space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
  CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
          blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}
func gradient(_ stops: [(UInt32, CGFloat, CGFloat)]) -> CGGradient {
  CGGradient(colorsSpace: space, colors: stops.map { color($0.0, $0.1) } as CFArray,
             locations: stops.map { $0.2 })!
}

// The macOS icon grid: an 824-point rounded square in the middle, with a soft shadow.
let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
let tilePath = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: color(0x000000, 0.35))
ctx.addPath(tilePath)
ctx.setFillColor(color(0x10163a))
ctx.fillPath()
ctx.restoreGState()

// Dusk sky: deep blue to violet-black, a warm glow low in the middle.
ctx.saveGState()
ctx.addPath(tilePath)
ctx.clip()
ctx.drawLinearGradient(gradient([(0x0b0f24, 1, 0), (0x1b2552, 1, 0.55), (0x34406f, 1, 1)]),
                       start: CGPoint(x: 512, y: 100), end: CGPoint(x: 512, y: 924), options: [])
ctx.drawRadialGradient(gradient([(0xffcf7a, 0.55, 0), (0xf0a04b, 0.18, 0.45), (0xf0a04b, 0, 1)]),
                       startCenter: CGPoint(x: 512, y: 330), startRadius: 0,
                       endCenter: CGPoint(x: 512, y: 330), endRadius: 470, options: [])
ctx.restoreGState()

// A thin gold border inside the edge.
let border = CGPath(roundedRect: tile.insetBy(dx: 30, dy: 30), cornerWidth: 157, cornerHeight: 157, transform: nil)
ctx.addPath(border)
ctx.setStrokeColor(color(0xe8b85a, 0.75))
ctx.setLineWidth(7)
ctx.strokePath()

// "II" in a classical serif, filled with a gold gradient.
let font = CTFontCreateWithName("Baskerville-Bold" as CFString, 600, nil)
let glyphsPath = CGMutablePath()
let line = CTLineCreateWithAttributedString(NSAttributedString(string: "II", attributes: [.font: font, .kern: 60]))
for run in CTLineGetGlyphRuns(line) as! [CTRun] {
  let count = CTRunGetGlyphCount(run)
  var glyphs = [CGGlyph](repeating: 0, count: count)
  var positions = [CGPoint](repeating: .zero, count: count)
  CTRunGetGlyphs(run, CFRange(location: 0, length: count), &glyphs)
  CTRunGetPositions(run, CFRange(location: 0, length: count), &positions)
  for i in 0..<count {
    if let glyph = CTFontCreatePathForGlyph(font, glyphs[i], nil) {
      glyphsPath.addPath(glyph, transform: CGAffineTransform(translationX: positions[i].x, y: positions[i].y))
    }
  }
}
let bounds = glyphsPath.boundingBoxOfPath
let text = CGMutablePath()
text.addPath(glyphsPath, transform: CGAffineTransform(translationX: 512 - bounds.midX, y: 512 - bounds.midY + 10))
ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: -14), blur: 22, color: color(0x000000, 0.6))
ctx.addPath(text)
ctx.setFillColor(color(0x6b4510))
ctx.fillPath()
ctx.restoreGState()
ctx.saveGState()
ctx.addPath(text)
ctx.clip()
let textBox = text.boundingBoxOfPath
ctx.drawLinearGradient(gradient([(0x8a5a14, 1, 0), (0xe2a83c, 1, 0.35), (0xffe9a8, 1, 0.62), (0xf3c45c, 1, 0.8),
                                 (0xb07a24, 1, 1)]),
                       start: CGPoint(x: 512, y: textBox.minY), end: CGPoint(x: 512, y: textBox.maxY), options: [])
ctx.restoreGState()
ctx.addPath(text)
ctx.setStrokeColor(color(0x5a3a0c, 0.9))
ctx.setLineWidth(4)
ctx.strokePath()

let image = ctx.makeImage()!
let rep = NSBitmapImageRep(cgImage: image)
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))

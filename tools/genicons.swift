#!/usr/bin/env swift
// Generate daily/hourly 1bpp Weather Icons subsets; no runtime scaling.
// Source, SHA256, OFL 1.1 and regeneration: third_party/weather-icons/README.md.

import CoreGraphics
import CoreText
import Foundation
import CryptoKit
import ImageIO

struct GlyphOutput {
    let codepoint: UInt32
    let bytes: [UInt8]
    let width: Int
    let height: Int
    let advance: Int
    let xoff: Int
    let yoff: Int
}

struct FontSpec {
    let name: String
    let size: CGFloat
    let snowSize: CGFloat
    let advance: Int
    let height: Int
}

struct Icon {
    let codepoint: UInt32
    let name: String
}

func fail(_ message: String) -> Never {
    fputs("genicons: \(message)\n", stderr)
    exit(1)
}

let args = CommandLine.arguments
if args.count != 3 && args.count != 4 {
    fail("usage: genicons.swift WEATHER_ICONS.ttf main/font_icons.c [PREVIEW.png]")
}

let fontURL = URL(fileURLWithPath: args[1])
let outputURL = URL(fileURLWithPath: args[2])
let expectedSHA256 = "176bda6661f213dde47c2114d76e476ec8ca9aae07dd54f9550d2d28fe02b4fd"
guard let fontData = try? Data(contentsOf: fontURL),
      SHA256.hash(data: fontData).map({ String(format: "%02x", $0) }).joined() == expectedSHA256 else {
    fail("input must be the pinned upstream TTF (SHA256 \(expectedSHA256))")
}
// Load the supplied bytes, not an installed font with a matching PostScript name.
guard let provider = CGDataProvider(data: fontData as CFData),
      let cgFont = CGFont(provider) else {
    fail("cannot load \(fontURL.path)")
}
let specs = [
    // Only wi-day-snow needs a smaller size: its sun + snow exceed the cell height.
    FontSpec(name: "font_icon_daily", size: 22, snowSize: 19, advance: 40, height: 27),
    FontSpec(name: "font_icon", size: 18, snowSize: 16, advance: 26, height: 23),
]

// Official CSS names at the pinned commit. Preview order is category order;
// C lookup tables are separately sorted by codepoint, as required by aafont.
let icons = [
    Icon(codepoint: 0xf00d, name: "wi-day-sunny"),
    Icon(codepoint: 0xf002, name: "wi-day-cloudy"),
    Icon(codepoint: 0xf013, name: "wi-cloudy"),
    Icon(codepoint: 0xf014, name: "wi-fog"),
    Icon(codepoint: 0xf00b, name: "wi-day-sprinkle"),
    Icon(codepoint: 0xf019, name: "wi-rain"),
    Icon(codepoint: 0xf01b, name: "wi-snow"),
    Icon(codepoint: 0xf01a, name: "wi-showers"),
    Icon(codepoint: 0xf00a, name: "wi-day-snow"), // Not F01D (wi-storm-showers).
    Icon(codepoint: 0xf01e, name: "wi-thunderstorm"),
    Icon(codepoint: 0xf015, name: "wi-hail"),
]
let sortedIcons = icons.sorted { $0.codepoint < $1.codepoint }

func glyphFor(_ codepoint: UInt32, _ ctFont: CTFont) -> CGGlyph {
    var character = UniChar(codepoint)
    var glyph = CGGlyph()
    guard CTFontGetGlyphsForCharacters(ctFont, &character, &glyph, 1) else {
        fail(String(format: "font has no glyph for U+%04X", codepoint))
    }
    return glyph
}

func render(_ codepoint: UInt32, _ spec: FontSpec) -> GlyphOutput {
    let size = codepoint == 0xf00a ? spec.snowSize : spec.size
    let ctFont = CTFontCreateWithGraphicsFont(cgFont, size, nil, nil)
    let glyph = glyphFor(codepoint, ctFont)
    guard let path = CTFontCreatePathForGlyph(ctFont, glyph, nil) else {
        fail(String(format: "cannot get path for U+%04X", codepoint))
    }
    let bbox = path.boundingBoxOfPath
    let padding: CGFloat = 4
    let canvasWidth = Int(ceil(bbox.width + padding * 2 + 4))
    let canvasHeight = Int(ceil(bbox.height + padding * 2 + 4))
    var pixels = [UInt8](repeating: 0xff, count: canvasWidth * canvasHeight)
    pixels.withUnsafeMutableBytes { buffer in
        guard let colorSpace = CGColorSpace(name: CGColorSpace.linearGray),
              let context = CGContext(data: buffer.baseAddress,
                                      width: canvasWidth,
                                      height: canvasHeight,
                                      bitsPerComponent: 8,
                                      bytesPerRow: canvasWidth,
                                      space: colorSpace,
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
            fail("cannot create bitmap context")
        }
        context.setShouldAntialias(false)
        context.setAllowsAntialiasing(false)
        context.setFillColor(gray: 0, alpha: 1)
        // Translate the entire outline into a padded canvas before cropping ink.
        context.translateBy(x: padding - bbox.origin.x, y: padding - bbox.origin.y)
        context.addPath(path)
        context.fillPath()
    }

    var minX = canvasWidth, minY = canvasHeight, maxX = -1, maxY = -1
    for y in 0..<canvasHeight {
        for x in 0..<canvasWidth where pixels[y * canvasWidth + x] < 128 {
            minX = min(minX, x)
            minY = min(minY, y)
            maxX = max(maxX, x)
            maxY = max(maxY, y)
        }
    }
    guard maxX >= minX && maxY >= minY else {
        fail(String(format: "blank glyph U+%04X", codepoint))
    }

    guard pixels.allSatisfy({ $0 == 0 || $0 == 255 }) else {
        fail("antialiased pixels found")
    }
    guard minX > 0 && minY > 0 && maxX < canvasWidth - 1 && maxY < canvasHeight - 1 else {
        fail(String(format: "raster canvas clips U+%04X", codepoint))
    }
    let width = maxX - minX + 1
    let height = maxY - minY + 1
    guard width <= spec.advance && height <= spec.height else {
        fail(String(format: "U+%04X: %dx%d exceeds %@ cell %dx%d",
                    codepoint, width, height, spec.name, spec.advance, spec.height))
    }
    let stride = (width + 7) / 8
    var bytes = [UInt8](repeating: 0, count: stride * height)
    // The bitmap context's memory rows already match CGImage's top-to-bottom
    // image rows, despite the path coordinates being Y-up. Do NOT reverse them.
    // Verified with the packed-data PNG: sun above cloud, precipitation below.
    // A change to the context/transform requires inspecting that preview again.
    for row in 0..<height {
        for col in 0..<width {
            let sourceY = minY + row
            if pixels[sourceY * canvasWidth + minX + col] < 128 {
                bytes[row * stride + col / 8] |= 0x80 >> (col & 7)
            }
        }
    }
    return GlyphOutput(codepoint: codepoint, bytes: bytes, width: width, height: height,
                       advance: spec.advance, xoff: (spec.advance - width) / 2,
                       yoff: -spec.height + (spec.height - height) / 2)
}

func hexBytes(_ bytes: [UInt8]) -> String {
    bytes.map { String(format: "0x%02x", $0) }.joined(separator: ",")
}

var output = """
// Generated by tools/genicons.swift; 1bpp, MSB first, byte-padded rows, top to bottom.
// Weather Icons by Erik Flowers, Lukas Bischoff (v1 Art), SIL OFL 1.1.
// Upstream: bb80982bf1f43f2d57f9dd753e7413bf88beb9ed
// TTF SHA256: \(expectedSHA256)
// License and source notices: third_party/weather-icons/{OFL.txt,README.md}.
#include "font_icons.h"


"""
var rendered: [[GlyphOutput]] = []
for spec in specs {
    let glyphs = sortedIcons.map { render($0.codepoint, spec) }
    // Distinct source glyphs must remain distinct at BOTH raster sizes.
    guard Set(glyphs.map { "\($0.width)x\($0.height):\(hexBytes($0.bytes))" }).count == icons.count else {
        fail("duplicate rasterized icons in \(spec.name)")
    }
    rendered.append(glyphs)
    var data: [UInt8] = []
    var records = ""
    for (icon, glyph) in zip(sortedIcons, glyphs) {
        records += "    {\(data.count), \(glyph.width), \(glyph.height), \(glyph.advance), "
        records += "\(glyph.xoff), \(glyph.yoff)}, // \(icon.name)\n"
        data.append(contentsOf: glyph.bytes)
        print(String(format: "%@: %@ U+%04X: %dx%d, advance=%d, xoff=%d, yoff=%d",
                     spec.name, icon.name, glyph.codepoint, glyph.width, glyph.height,
                     glyph.advance, glyph.xoff, glyph.yoff))
    }
    guard data.count <= Int(UInt16.max) else { fail("aaglyph offset overflow") }
    output += "static const uint8_t \(spec.name)_data[] = {\n"
    for start in stride(from: 0, to: data.count, by: 16) {
        output += "    " + hexBytes(Array(data[start..<min(start + 16, data.count)])) + ",\n"
    }
    output += "};\nstatic const aaglyph \(spec.name)_glyphs[] = {\n\(records)};\n"
    let cps = sortedIcons.map { String(format: "0x%04x", $0.codepoint) }.joined(separator: ", ")
    output += "static const uint16_t \(spec.name)_cps[] = {\(cps)};\n\n"
    output += "const aafont \(spec.name) = {\(spec.name)_data, \(spec.name)_glyphs, "
    output += "\(spec.name)_cps, \(glyphs.count), \(spec.height)};\n\n"
    print("\(spec.name): \(data.count) bitmap bytes, \(glyphs.count) glyphs")
}

// Decode the exact packed data and offsets emitted above, not a second font render.
// Top: daily; bottom: hourly. Left to right: the category order in `icons`.
// Gray borders mark cells (not part of the generated glyphs); pixels enlarged 4x.
func writePreview(_ url: URL) {
    let scale = 4, slotW = 44, slotH = 33
    let width = slotW * icons.count * scale, height = slotH * specs.count * scale
    var pixels = [UInt8](repeating: 255, count: width * height)
    func put(_ x: Int, _ y: Int, _ gray: UInt8) {
        for dy in 0..<scale {
            for dx in 0..<scale {
                pixels[(y * scale + dy) * width + x * scale + dx] = gray
            }
        }
    }
    for (row, spec) in specs.enumerated() {
        for (column, icon) in icons.enumerated() {
            let glyph = rendered[row].first { $0.codepoint == icon.codepoint }!
            let left = column * slotW + (slotW - spec.advance) / 2
            let top = row * slotH + 3
            for x in (left - 1)...(left + spec.advance) {
                put(x, top - 1, 180)
                put(x, top + spec.height, 180)
            }
            for y in top..<(top + spec.height) {
                put(left - 1, y, 180)
                put(left + spec.advance, y, 180)
            }
            let stride = (glyph.width + 7) / 8
            for y in 0..<glyph.height {
                for x in 0..<glyph.width where glyph.bytes[y * stride + x / 8] & (0x80 >> (x & 7)) != 0 {
                    put(left + glyph.xoff + x, top + spec.height + glyph.yoff + y, 0)
                }
            }
        }
    }
    guard let provider = CGDataProvider(data: Data(pixels) as CFData),
          let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8,
                              bytesPerRow: width, space: CGColorSpaceCreateDeviceGray(),
                              bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                              provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
          let destination = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else {
        fail("cannot create preview \(url.path)")
    }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { fail("cannot write preview \(url.path)") }
    print("Preview: \(url.path)")
}

if args.count == 4 { writePreview(URL(fileURLWithPath: args[3])) }
do {
    try output.write(to: outputURL, atomically: true, encoding: .utf8)
} catch {
    fail("cannot write \(outputURL.path): \(error)")
}

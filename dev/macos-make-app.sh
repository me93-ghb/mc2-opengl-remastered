#!/bin/bash
# Install ~/Applications/MC2.app: a Dock/Spotlight launcher that runs
# dev/macos-play.sh (build the latest code, then play). Re-run after moving
# the checkout; the app points at this checkout by absolute path.
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP="$HOME/Applications/MC2.app"
ICO="$REPO/../MechCommander2-Source/Source/MechCommander2.ico"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>MC2</string>
	<key>CFBundleDisplayName</key><string>MechCommander 2</string>
	<key>CFBundleExecutable</key><string>mc2launch</string>
	<key>CFBundleIdentifier</key><string>dev.ojhudson.mc2</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>CFBundleIconFile</key><string>mc2</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

cat > "$APP/Contents/MacOS/mc2launch" <<EOF
#!/bin/bash
exec "$REPO/dev/macos-play.sh" "\$@"
EOF
chmod +x "$APP/Contents/MacOS/mc2launch"

# Icon: the retail 32x32 icon, scaled up with hard pixel edges (no blur).
if [ -f "$ICO" ] && command -v swift >/dev/null; then
    TMP="$(mktemp -d)"
    swift - "$ICO" "$TMP/mc2.iconset" <<'SWIFT'
import AppKit
let args = CommandLine.arguments
let src = NSImage(contentsOfFile: args[1])!
var rect = NSRect(x: 0, y: 0, width: 32, height: 32)
let cg = src.cgImage(forProposedRect: &rect, context: nil, hints: nil)!
try! FileManager.default.createDirectory(atPath: args[2], withIntermediateDirectories: true)
for (size, name) in [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"),
                     (128, "128x128"), (256, "128x128@2x"), (256, "256x256"),
                     (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")] {
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .none
    ctx.draw(cg, in: CGRect(x: 0, y: 0, width: size, height: size))
    let png = NSBitmapImageRep(cgImage: ctx.makeImage()!).representation(using: .png, properties: [:])!
    try! png.write(to: URL(fileURLWithPath: "\(args[2])/icon_\(name).png"))
}
SWIFT
    iconutil -c icns "$TMP/mc2.iconset" -o "$APP/Contents/Resources/mc2.icns"
    rm -rf "$TMP"
fi

# Drop the old pre-launcher backup and make Finder/Dock pick up the new icon.
rm -f "$APP/Contents/MacOS/mc2launch.bak-"*
touch "$APP"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"
echo "Installed $APP"

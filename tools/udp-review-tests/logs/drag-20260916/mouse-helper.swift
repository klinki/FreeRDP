import Foundation
import CoreGraphics
let args = CommandLine.arguments
func report(_ phase:String) {
    let p = CGEvent(source:nil)!.location
    print("\(Date().timeIntervalSince1970) \(phase) cursor=\(p.x),\(p.y) down=\(CGEventSource.buttonState(.combinedSessionState, button:.left))")
    fflush(stdout)
}
if args.count < 2 { exit(2) }
if args[1] == "permissions" { print("canPostMouseEvents=\(CGPreflightPostEventAccess())") } else if args[1] == "windows" {
    let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly,.excludeDesktopElements],kCGNullWindowID) as? [[String:Any]] ?? []
    for w in windows where (w[kCGWindowOwnerName as String] as? String ?? "").contains("sdl-freerdp") || (w[kCGWindowOwnerName as String] as? String ?? "").contains("FreeRDP") { print(w) }
} else if args[1] == "displays" {
    var ids = [CGDirectDisplayID](repeating:0,count:16)
    var count:UInt32 = 0
    CGGetActiveDisplayList(16, &ids, &count)
    for id in ids.prefix(Int(count)) {
        let b = CGDisplayBounds(id)
        let m = CGDisplayCopyDisplayMode(id)!
        print("display=\(id) builtin=\(CGDisplayIsBuiltin(id)) bounds=\(b) mode=\(m.width)x\(m.height) pixels=\(m.pixelWidth)x\(m.pixelHeight)")
        let modes = CGDisplayCopyAllDisplayModes(id,nil) as? [CGDisplayMode] ?? []
        print("modes=" + modes.map { "\($0.width)x\($0.height)/\($0.pixelWidth)x\($0.pixelHeight)" }.joined(separator:" "))
    }
} else if args[1] == "position" { report("position") }
else if args[1] == "move" && args.count == 4 {
 let p=CGPoint(x:Double(args[2])!,y:Double(args[3])!)
 print("warpResult=\(CGWarpMouseCursorPosition(p).rawValue)"); Thread.sleep(forTimeInterval:0.3); report("move")
}
else if args[1] == "drag" && args.count == 9 {
    let start=CGPoint(x:Double(args[2])!,y:Double(args[3])!)
    let end=CGPoint(x:Double(args[4])!,y:Double(args[5])!)
    let duration=Double(args[6])!, pause=Double(args[7])!, back=Double(args[8])!
    func post(_ kind:CGEventType,_ point:CGPoint) {
        CGEvent(mouseEventSource:CGEventSource(stateID:.hidSystemState),mouseType:kind,mouseCursorPosition:point,mouseButton:.left)!.post(tap:.cgSessionEventTap)
    }
    func move(_ a:CGPoint,_ b:CGPoint,_ seconds:Double) {
        let steps=max(1,Int(seconds*60))
        for i in 1...steps { let t=Double(i)/Double(steps); post(.leftMouseDragged,CGPoint(x:a.x+(b.x-a.x)*t,y:a.y+(b.y-a.y)*t)); usleep(useconds_t(seconds/Double(steps)*1_000_000)) }
    }
    post(.mouseMoved,start); usleep(200000); post(.leftMouseDown,start); report("down")
    move(start,end,duration); report("pause-start"); Thread.sleep(forTimeInterval:pause); report("pause-end")
    if back > 0 { move(end,start,back); post(.leftMouseUp,start) } else { post(.leftMouseUp,end) }
    report("released")
} else { exit(2) }

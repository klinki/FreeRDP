// macOS-only ScreenCaptureKit marker observer and CGEvent input injector.
// Compile locally: swiftc -O -parse-as-library BenchObserver.swift -o bench-observer
// Needs Screen Recording + Accessibility permissions. Pinned US input layout required
// for the physical-key probe path. Capture receipt is NOT physical photon time.
import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreVideo
import CoreGraphics
import AppKit
import ApplicationServices

let outputLock = NSLock()
let observerTargetPID: pid_t? = {
    let args = CommandLine.arguments
    guard let index = args.firstIndex(of:"--target-pid"), index+1 < args.count,
          let pid = Int32(args[index+1]), pid > 0 else {return nil}
    return pid
}()
func emit(_ input: [String: Any]) {
    var x=input
    if x["t_ns"] == nil { x["t_ns"] = DispatchTime.now().uptimeNanoseconds }
    guard let d=try? JSONSerialization.data(withJSONObject:x,options:[.sortedKeys]) else {return}
    outputLock.lock();FileHandle.standardOutput.write(d);FileHandle.standardOutput.write(Data([10]));outputLock.unlock()
}
func crc16(_ data:[UInt8]) -> UInt16 {
    var crc:UInt16=0xffff
    for b in data { crc ^= UInt16(b)<<8; for _ in 0..<8 {crc = (crc & 0x8000) != 0 ? (crc &<< 1)^0x1021 : crc &<< 1} }
    return crc
}
func readBE(_ a:[UInt8],_ offset:Int,_ count:Int) -> UInt32 {
    var x:UInt32=0; for i in offset..<(offset+count) {x=(x<<8)|UInt32(a[i])};return x
}
final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    var last:String="";var count=0;var valid=0
    func stream(_ stream: SCStream, didStopWithError error: Error) {emit(["type":"observer_error","error":String(describing:error)])}
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of outputType: SCStreamOutputType) {
        guard outputType == .screen, sb.isValid, let pixel=sb.imageBuffer else { return }
        let now=DispatchTime.now().uptimeNanoseconds; count+=1
        CVPixelBufferLockBaseAddress(pixel,.readOnly);defer{CVPixelBufferUnlockBaseAddress(pixel,.readOnly)}
        guard CVPixelBufferGetWidth(pixel)==112,CVPixelBufferGetHeight(pixel)==64,let base=CVPixelBufferGetBaseAddress(pixel) else {return}
        let stride=CVPixelBufferGetBytesPerRow(pixel),p=base.assumingMemoryBound(to:UInt8.self)
        var a=[UInt8](repeating:0,count:16)
        for bit in 0..<128 {
            let x=8+(bit%16)*6+3,y=8+(bit/16)*6+3
            var luminance=0
            for dy in -1...1 {for dx in -1...1 {let q=(y+dy)*stride+(x+dx)*4;luminance += Int(p[q])+Int(p[q+1])+Int(p[q+2])}}
            let value=luminance/27
            if value>80 && value<175 {return} // ambiguous cell: no guessed bit
            if value>=175 {a[bit/8] |= UInt8(1 << (7-bit%8))}
        }
        guard readBE(a,0,2)==0xb17e,crc16(Array(a[0..<14]))==UInt16(readBE(a,14,2)) else {return}
        valid+=1
        let key=a.map{String(format:"%02x",$0)}.joined()
        if key != last {
            emit(["type":"marker","t_ns":now,"run_tag":readBE(a,2,2),"phase_id":readBE(a,4,2),"tick":readBE(a,6,4),"probe_id":readBE(a,10,4),"capture_frames":count,"valid_frames":valid,
                  "timestamp_source":"capture_callback_monotonic_ns"]);last=key
        }
    }
}
func key(_ code:CGKeyCode,_ shift:Bool=false) {
    for down in [true,false] {
        guard let e=CGEvent(keyboardEventSource:nil,virtualKey:code,keyDown:down) else {continue}
        if shift {e.flags = .maskShift}
        if let pid = observerTargetPID {e.postToPid(pid)}
    }
}
func shiftKey(_ down: Bool) {
    guard let event = CGEvent(keyboardEventSource:nil,virtualKey:56,keyDown:down) else {return}
    event.type = .flagsChanged
    event.flags = down ? .maskShift : []
    if let pid = observerTargetPID {event.postToPid(pid)}
}
func foregroundFields() -> [String: Any] {
    let target = observerTargetPID
    // Keep one foreground snapshot. A second Launch Services lookup can briefly
    // return nil even while that exact PID is the foreground application.
    let frontApp = NSWorkspace.shared.frontmostApplication
    let front = frontApp?.processIdentifier
    let app = target.flatMap {pid in
        frontApp?.processIdentifier == pid ? frontApp : NSRunningApplication(processIdentifier: pid)
    }
    let foreground = target != nil && front == target && app != nil && app?.isTerminated == false && app?.isActive == true
    return ["target_pid":target.map{$0 as Any} ?? NSNull(),
            "frontmost_pid":front.map{$0 as Any} ?? NSNull(),
            "target_exists":app != nil,
            "target_terminated":app.map{$0.isTerminated as Any} ?? NSNull(),
            "target_active":app.map{$0.isActive as Any} ?? NSNull(),
            "target_foreground":foreground]
}
func replyForegroundStatus(_ request: String) {
    var status = foregroundFields()
    status["type"] = "foreground_status"
    status["request_id"] = request
    emit(status)
}
func probe(_ id:UInt32,_ scheduled:UInt64?) {
    var focus = foregroundFields()
    guard focus["target_foreground"] as? Bool == true else {
        focus["type"] = "probe_error"
        focus["probe_id"] = id
        focus["error"] = "Owned RDP client is not foreground; input was not posted"
        emit(focus)
        return
    }
    let codes:[Character:CGKeyCode]=["0":29,"1":18,"2":19,"3":20,"4":21,"5":23,"6":22,"7":26,"8":28,"9":25,"a":0,"b":11,"c":8,"d":2,"e":14,"f":3]
    let now=DispatchTime.now().uptimeNanoseconds
    emit(["type":"probe_injected","probe_id":id,"t_ns":now,"scheduled_ns":scheduled ?? now])
    // SDL forwards modifier transitions separately from printable key flags.
    // A Shift flag attached only to the "1" event can arrive as plain "1".
    shiftKey(true)
    key(18,true) // ! in the pinned US layout
    shiftKey(false)
    for c in String(format:"%08x",id) {if let code=codes[c] {key(code)}}
    key(36) // Return
    emit(["type":"probe_injection_finished","probe_id":id])
}
@main struct Main {
    static func main() async throws {
        let args=CommandLine.arguments
        func arg(_ key:String,_ fallback:String) -> String {
            guard let i=args.firstIndex(of:key),i+1<args.count else{return fallback};return args[i+1]
        }
        // Diagnostic mode exercises the same stdin RPC without screen capture
        // or input, so its namespace/timeout contract can be checked safely.
        if args.contains("--foreground-status-only") {
            if let line = readLine(), let data = line.data(using: .utf8),
               let command = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
               command["op"] as? String == "foreground_status",
               let request = command["request_id"] as? String, !request.isEmpty && request.count <= 64 {
                replyForegroundStatus(request)
            }
            return
        }
        if args.contains("--check-environment") {
            var activeIDs = [CGDirectDisplayID](repeating: 0, count: 128)
            var activeCount: UInt32 = 0
            let displayResult = CGGetActiveDisplayList(UInt32(activeIDs.count), &activeIDs, &activeCount)
            var activeDisplays: [[String: Any]] = []
            var inventoryValid = displayResult == .success && activeCount <= activeIDs.count
            if inventoryValid {
                for displayID in activeIDs.prefix(Int(activeCount)) {
                    guard CGDisplayIsActive(displayID) != 0,
                          let uuid = CGDisplayCreateUUIDFromDisplayID(displayID),
                          let text = CFUUIDCreateString(kCFAllocatorDefault, uuid.takeRetainedValue()) else {
                        inventoryValid = false
                        continue
                    }
                    activeDisplays.append(["display_id": displayID, "uuid": text as String])
                }
            }
            let session = CGSessionCopyCurrentDictionary() as? [String:Any] ?? [:]
            func flag(_ name: String) -> Bool? {(session[name] as? NSNumber)?.boolValue}
            let locked = flag("CGSSessionScreenIsLocked")
            let console = flag("kCGSSessionOnConsoleKey")
            let loggedIn = flag("kCGSessionLoginDoneKey")
            let front = NSWorkspace.shared.frontmostApplication
            let loginWindow = front?.bundleIdentifier == "com.apple.loginwindow" || front?.localizedName == "loginwindow"
            let capture = CGPreflightScreenCaptureAccess()
            let access = AXIsProcessTrusted()
            let available = console == true && loggedIn == true && locked != true && front != nil && !loginWindow && capture && access && inventoryValid && !activeDisplays.isEmpty
            emit(["type":"observer_environment","available":available,
                  "screen_locked":locked.map{$0 as Any} ?? NSNull(),
                  "console_session":console.map{$0 as Any} ?? NSNull(),
                  "login_complete":loggedIn.map{$0 as Any} ?? NSNull(),
                  "login_window_foreground":loginWindow,"capture_allowed":capture,
                  "accessibility_allowed":access,
                  "display_inventory_valid":inventoryValid,"active_displays":activeDisplays,
                  "scope":"Desktop prerequisite check; CRC and targeted-input roundtrip are still required"])
            return
        }
        let content=try await SCShareableContent.excludingDesktopWindows(false,onScreenWindowsOnly:true)
        if args.contains("--list") {for d in content.displays {emit(["type":"display","display_id":d.displayID,"width":d.width,"height":d.height,"frame":NSStringFromRect(d.frame)])};return}
        let displayId=UInt32(arg("--display","0")) ?? 0
        guard let d=content.displays.first(where:{$0.displayID==displayId}) else {throw NSError(domain:"Choose --display from --list",code:1)}
        let rect=arg("--rect","").split(separator:",").compactMap{Double($0)}
        guard rect.count==4,rect[2]>0,rect[3]>0 else {throw NSError(domain:"Need --rect x,y,w,h of marker, display-relative points",code:2)}
        let cfg=SCStreamConfiguration();cfg.width=112;cfg.height=64
        cfg.sourceRect=CGRect(x:rect[0],y:rect[1],width:rect[2],height:rect[3])
        cfg.pixelFormat=kCVPixelFormatType_32BGRA;cfg.showsCursor=false;cfg.queueDepth=3
        cfg.minimumFrameInterval=CMTime(value:1,timescale:Int32(arg("--fps","120")) ?? 120)
        let collector=Capture();let filter=SCContentFilter(display:d,excludingWindows:[])
        let stream=SCStream(filter:filter,configuration:cfg,delegate:collector)
        let captureQueue=DispatchQueue(label:"rdpbench.capture")
        try stream.addStreamOutput(collector,type:.screen,sampleHandlerQueue:captureQueue)
        try await stream.startCapture();emit(["type":"observer_ready","display_id":d.displayID,"target_pid":observerTargetPID ?? 0,"input_scope":"Targeted events to the owned foreground RDP client","clock":"DispatchTime.uptimeNanoseconds","measurement":"input to compositor-capture callback (not photons)"])
        DispatchQueue.global(qos:.userInitiated).async {
            while let line=readLine() {
                guard let data=line.data(using:.utf8),let x=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] else {continue}
                if x["op"] as? String == "foreground_status", let request = x["request_id"] as? String,
                   !request.isEmpty && request.count <= 64 {
                    replyForegroundStatus(request)
                }
                if x["op"] as? String == "probe",let n=x["probe_id"] as? NSNumber {probe(n.uint32Value,(x["scheduled_ns"] as? NSNumber)?.uint64Value)}
                if x["op"] as? String == "stop" {exit(0)}
            }
            exit(0)
        }
        while true {
            try await Task.sleep(nanoseconds:1_000_000_000)
            emit(["type":"observer_heartbeat"])
        }
    }
}

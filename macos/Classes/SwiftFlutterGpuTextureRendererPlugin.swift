import Cocoa
import FlutterMacOS
import CoreVideo

// The engine pulls the current CVPixelBuffer through copyPixelBuffer; the
// decoder hands down IOSurface ids (zero-copy) via the C API.
@objc public class GpuTextureOutput: NSObject, FlutterTexture {
    public var textureId: Int64 = -1
    private var registry: FlutterTextureRegistry?
    private var ioSurfaceId: UInt32 = 0
    private let queue = DispatchQueue(label: "gpu_texture_output_sync_queue")

    public static func new(registry: FlutterTextureRegistry?) -> GpuTextureOutput {
        let output = GpuTextureOutput()
        output.registry = registry
        output.textureId = registry?.register(output) ?? -1
        return output
    }

    @objc public func markFrameAvaliable(id: UInt32) -> Bool {
        queue.sync {
            ioSurfaceId = id
        }
        registry?.textureFrameAvailable(textureId)
        return true
    }

    public func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
        var pixelBuffer: Unmanaged<CVPixelBuffer>?
        queue.sync {
            let surfaceId = ioSurfaceId
            if surfaceId == 0 {
                return
            }
            // The decoder's mailbox may have released the surface already;
            // a failed lookup means the engine skips the frame.
            guard let ioSurface = IOSurfaceLookup(IOSurfaceID(surfaceId)) else {
                return
            }
            // The create hands back a +1 reference for the engine to own.
            CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nil as CFDictionary?, &pixelBuffer)
            // IOSurfaceRef is a managed CF type here; ARC drops the
            // lookup's +1 at scope exit.
        }
        return pixelBuffer
    }

    public func unregister() {
        registry?.unregisterTexture(textureId)
        registry = nil
    }
}

// One instance per flutter engine (each registrar registers its own), so
// outputs die with their engine instead of living in a global map. The C API
// addresses outputs by object pointer, which stays valid across engines.
public class FlutterGpuTextureRendererPlugin: NSObject, FlutterPlugin {
    private var outputs: [Int64: GpuTextureOutput] = [:]
    private var textureRegistry: FlutterTextureRegistry?

    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(
            name: "flutter_gpu_texture_renderer", binaryMessenger: registrar.messenger)
        registrar.addMethodCallDelegate(
            FlutterGpuTextureRendererPlugin(registrar: registrar), channel: channel)
    }

    init(registrar: FlutterPluginRegistrar) {
        textureRegistry = registrar.textures
        super.init()
    }

    deinit {
        for output in outputs.values {
            output.unregister()
        }
        outputs.removeAll()
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "registerTexture":
            let output = GpuTextureOutput.new(registry: textureRegistry)
            outputs[output.textureId] = output
            result(output.textureId)
        case "unregisterTexture":
            let args = call.arguments as! [String: Any]
            let id = args["id"] as! Int64
            if let output = outputs.removeValue(forKey: id) {
                output.unregister()
                result(true)
            } else {
                result(false)
            }
        case "output":
            let args = call.arguments as! [String: Any]
            let id = args["id"] as! Int64
            let output = outputs[id]
            if output == nil {
                result(0)
            } else {
                let unmanaged = Unmanaged.passUnretained(output!)
                result(UInt(bitPattern: unmanaged.toOpaque()))
            }
        case "fps":
            result(Int16(0))
        default:
            result(FlutterMethodNotImplemented)
        }
    }
}

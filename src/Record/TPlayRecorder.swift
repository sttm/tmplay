import AVFoundation
import CoreMedia
import Darwin
import Foundation
import ScreenCaptureKit

final class DesktopRecorder: NSObject, SCStreamOutput {
    private var stream: SCStream?
    private let output: URL
    private var handle: FileHandle?
    private var dataBytes: UInt32 = 0

    init(output: URL) {
        self.output = output
    }

    func start() async throws {
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            throw NSError(domain: "tmplay", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Grant Screen Recording permission, then start recording again."])
        }
        let content = try await SCShareableContent.current
        guard let display = content.displays.first else {
            throw NSError(domain: "tmplay", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "No display available for desktop capture."])
        }
        FileManager.default.createFile(atPath: output.path, contents: nil)
        handle = try FileHandle(forWritingTo: output)
        try handle?.write(contentsOf: wavHeader(dataSize: 0))

        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        let config = SCStreamConfiguration()
        config.capturesAudio = true
        config.sampleRate = 48_000
        config.channelCount = 2
        let stream = SCStream(filter: filter, configuration: config, delegate: nil)
        try stream.addStreamOutput(self, type: .audio,
                                   sampleHandlerQueue: DispatchQueue(label: "tmplay.recorder.audio", qos: .userInitiated))
        self.stream = stream
        try await stream.startCapture()
    }

    func stop() async {
        try? await stream?.stopCapture()
        guard let handle else { return }
        try? handle.seek(toOffset: 0)
        try? handle.write(contentsOf: wavHeader(dataSize: dataBytes))
        try? handle.close()
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of outputType: SCStreamOutputType) {
        guard outputType == .audio, CMSampleBufferDataIsReady(sampleBuffer) else { return }
        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        guard frames > 0 else { return }

        var retainedBlock: CMBlockBuffer?
        // ScreenCaptureKit delivers Float32 planar audio. Convert it to the
        // declared PCM WAV format instead of copying opaque sample bytes.
        let listSize = MemoryLayout<AudioBufferList>.size + MemoryLayout<AudioBuffer>.size
        let rawList = UnsafeMutableRawPointer.allocate(
            byteCount: listSize,
            alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { rawList.deallocate() }
        let audioList = rawList.assumingMemoryBound(to: AudioBufferList.self)
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: audioList,
            bufferListSize: listSize,
            blockBufferAllocator: kCFAllocatorDefault,
            blockBufferMemoryAllocator: kCFAllocatorDefault,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            blockBufferOut: &retainedBlock) == noErr else { return }

        let buffers = UnsafeMutableAudioBufferListPointer(audioList)
        guard let left = buffers.first?.mData?.assumingMemoryBound(to: Float.self) else { return }
        let right = buffers.count > 1
            ? buffers[1].mData?.assumingMemoryBound(to: Float.self)
            : left
        guard let right else { return }

        var pcm = [Int16](repeating: 0, count: frames * 2)
        for index in 0..<frames {
            pcm[index * 2] = Int16(max(-1, min(1, left[index])) * 32767)
            pcm[index * 2 + 1] = Int16(max(-1, min(1, right[index])) * 32767)
        }
        pcm.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            try? handle?.write(contentsOf: Data(bytes: base, count: bytes.count))
        }
        dataBytes &+= UInt32(pcm.count * MemoryLayout<Int16>.size)
    }

    private func wavHeader(dataSize: UInt32) -> Data {
        var data = Data()
        func append<T: FixedWidthInteger>(_ value: T) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        data.append("RIFF".data(using: .ascii)!)
        append(UInt32(36 + dataSize))
        data.append("WAVEfmt ".data(using: .ascii)!)
        append(UInt32(16)); append(UInt16(1)); append(UInt16(2)); append(UInt32(48_000))
        append(UInt32(48_000 * 2 * 2)); append(UInt16(4)); append(UInt16(16))
        data.append("data".data(using: .ascii)!); append(dataSize)
        return data
    }
}

let outputPath = CommandLine.arguments.dropFirst().first ??
    URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("desktop.wav").path
let output = URL(fileURLWithPath: outputPath)
let recorder = DesktopRecorder(output: output)
let interruptSignal = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
interruptSignal.setEventHandler {
    Task {
        await recorder.stop()
        print(output.path)
        exit(0)
    }
}
interruptSignal.resume()
Darwin.signal(SIGINT, SIG_IGN)
Task {
    do {
        try await recorder.start()
        print("RECORDING \(output.path)")
    } catch {
        FileHandle.standardError.write(Data("\(error.localizedDescription)\n".utf8))
        exit(1)
    }
}
dispatchMain()

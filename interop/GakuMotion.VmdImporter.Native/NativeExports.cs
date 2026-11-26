namespace GakuMotion.VmdImporter.Native;

using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using GakuMotion.VmdImporter.Native.Camera;
using GakuMotion.VmdImporter.Native.Facial;

/// <summary>
/// Native ABI consumed by GakuMotion.dll (GkCsharpImport.hpp).
///
///   int32_t GkVmdImport_Run(const uint8_t* requestUtf8Json,
///                           uint8_t* responseUtf8Json, int32_t responseCapacity);
///
/// Return codes:  0 = success, 1 = import failed (see response), 2 = invalid
/// argument, 3 = response buffer too small, 4 = busy (one import at a time).
/// No managed exception may cross this boundary.
/// </summary>
internal static unsafe class NativeExports
{
    private const int Ok = 0;
    private const int Failed = 1;
    private const int InvalidArgument = 2;
    private const int BufferTooSmall = 3;
    private const int Busy = 4;

    private static int _running;

    /// <summary>
    /// ABI v4 also preserves source frame rate and rebake requests. Older modules would
    /// silently drop those fields, so the plugin refuses to pair with them.
    /// </summary>
    [UnmanagedCallersOnly(EntryPoint = "GkVmdImporter_AbiVersion")]
    public static int AbiVersion() => 4;

    [UnmanagedCallersOnly(EntryPoint = "GkCameraImporter_AbiVersion")]
    public static int CameraAbiVersion() => 1;

    [UnmanagedCallersOnly(EntryPoint = "GkMouthSource_AbiVersion")]
    public static int MouthSourceAbiVersion() => 1;

    [UnmanagedCallersOnly(EntryPoint = "GkMouthSource_Read")]
    public static int ReadMouthSource(byte* sourceUtf8, float sourceFps, float clipFps,
        int frames, float* outputFrameMajor, int outputFloats) =>
        ReadMouthSourceChannels(sourceUtf8, sourceFps, clipFps, frames, outputFrameMajor, outputFloats, extraOnly: false);

    [UnmanagedCallersOnly(EntryPoint = "GkMouthSourceExtra_AbiVersion")]
    public static int MouthSourceExtraAbiVersion() => 1;

    /// <summary>Returns present, にやり2, 口角下げ and residual unsupported values in frame-major order.</summary>
    [UnmanagedCallersOnly(EntryPoint = "GkMouthSourceExtra_Read")]
    public static int ReadMouthSourceExtra(byte* sourceUtf8, float sourceFps, float clipFps,
        int frames, float* outputFrameMajor, int outputFloats) =>
        ReadMouthSourceChannels(sourceUtf8, sourceFps, clipFps, frames, outputFrameMajor, outputFloats, extraOnly: true);

    private static int ReadMouthSourceChannels(byte* sourceUtf8, float sourceFps, float clipFps,
        int frames, float* outputFrameMajor, int outputFloats, bool extraOnly)
    {
        if (sourceUtf8 == null || outputFrameMajor == null ||
            !MouthSourceReader.ValidDimensions(sourceFps, clipFps, frames)) return InvalidArgument;
        int channelCount = extraOnly ? MouthSourceReader.ExtraChannelCount : MouthSourceReader.ChannelCount;
        if (outputFloats < frames * channelCount) return BufferTooSmall;
        try
        {
            string? sourcePath = Marshal.PtrToStringUTF8((nint)sourceUtf8);
            if (string.IsNullOrWhiteSpace(sourcePath)) return InvalidArgument;
            float[] values = MouthSourceReader.Read(sourcePath, sourceFps, clipFps, frames, extraOnly);
            values.AsSpan().CopyTo(new Span<float>(outputFrameMajor, outputFloats));
            return Ok;
        }
        catch (Exception)
        {
            return Failed;
        }
    }

    /// <summary>Camera-only ABI v1; shares the body import gate to avoid concurrent writes.</summary>
    [UnmanagedCallersOnly(EntryPoint = "GkCameraImport_Run")]
    public static int RunCamera(byte* requestUtf8Json, byte* responseUtf8Json, int responseCapacity)
    {
        if (requestUtf8Json == null || responseUtf8Json == null || responseCapacity <= 0)
            return InvalidArgument;
        if (Interlocked.CompareExchange(ref _running, 1, 0) != 0)
        {
            WriteResponse(responseUtf8Json, responseCapacity, false, null, "一次只允许一个导入任务", "busy", 0);
            return Busy;
        }
        try
        {
            ReadOnlySpan<byte> json = MemoryMarshal.CreateReadOnlySpanFromNullTerminated(requestUtf8Json);
            CameraImportRequest request = CameraImportRequest.Parse(json);
            using var log = new ImportLog(request.LogPath);
            ImportOutcome outcome = CameraImportPipeline.Run(request, log);
            int written = WriteResponse(responseUtf8Json, responseCapacity, outcome.Ok, outcome,
                outcome.Error, outcome.ErrorCode, outcome.Timing.TotalMs);
            if (written != 0) return BufferTooSmall;
            return outcome.Ok ? Ok : Failed;
        }
        catch (Exception ex)
        {
            if (WriteResponse(responseUtf8Json, responseCapacity, false, null,
                ex.GetType().Name + ": " + ex.Message, "bad_request", 0) != 0)
                return BufferTooSmall;
            return Failed;
        }
        finally
        {
            Volatile.Write(ref _running, 0);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "GkVmdImport_Run")]
    public static int Run(byte* requestUtf8Json, byte* responseUtf8Json, int responseCapacity)
    {
        if (requestUtf8Json == null || responseUtf8Json == null || responseCapacity <= 0)
            return InvalidArgument;
        if (Interlocked.CompareExchange(ref _running, 1, 0) != 0)
        {
            WriteResponse(responseUtf8Json, responseCapacity, ok: false, null, "一次只允许一个导入任务", "busy", 0);
            return Busy;
        }

        try
        {
            ReadOnlySpan<byte> json = MemoryMarshal.CreateReadOnlySpanFromNullTerminated(requestUtf8Json);
            ImportRequest request;
            try
            {
                request = ImportRequest.Parse(json);
            }
            catch (Exception ex)
            {
                if (WriteResponse(responseUtf8Json, responseCapacity, ok: false, null,
                    "导入请求解析失败: " + ex.Message, "bad_request", 0) != 0)
                    return BufferTooSmall;
                return Failed;
            }

            using var log = new ImportLog(request.LogPath);
            ImportOutcome outcome = ImportPipeline.Run(request, log);
            int written = WriteResponse(responseUtf8Json, responseCapacity, outcome.Ok, outcome,
                outcome.Error, outcome.ErrorCode, outcome.Timing.TotalMs);
            if (written != 0) return BufferTooSmall;
            return outcome.Ok ? Ok : Failed;
        }
        catch (Exception ex)
        {
            if (WriteResponse(responseUtf8Json, responseCapacity, ok: false, null,
                ex.GetType().Name + ": " + ex.Message, "internal", 0) != 0)
                return BufferTooSmall;
            return Failed;
        }
        finally
        {
            Volatile.Write(ref _running, 0);
        }
    }

    /// <summary>Returns 0 when the payload fit, -1 when it was truncated.</summary>
    private static int WriteResponse(byte* destination, int capacity, bool ok, ImportOutcome? outcome,
        string error, string code, double totalMs)
    {
        var sb = new StringBuilder(256);
        sb.Append('{');
        sb.Append("\"ok\":").Append(ok ? "true" : "false");
        sb.Append(",\"dir\":");
        AppendQuoted(sb, outcome?.Directory ?? string.Empty);
        sb.Append(",\"frames\":").Append((outcome?.Timing.Frames ?? 0).ToString(CultureInfo.InvariantCulture));
        sb.Append(",\"bones\":").Append((outcome?.Timing.Bones ?? 0).ToString(CultureInfo.InvariantCulture));
        sb.Append(",\"clip_bytes\":").Append((outcome?.Timing.ClipBytes ?? 0).ToString(CultureInfo.InvariantCulture));
        sb.Append(",\"total_ms\":").Append(totalMs.ToString("F1", CultureInfo.InvariantCulture));
        sb.Append(",\"candidate_b_tree\":\"").Append(ImportPipeline.CandidateBTreeSha256).Append('"');
        sb.Append(",\"code\":");
        AppendQuoted(sb, code);
        sb.Append(",\"error\":");
        AppendQuoted(sb, error);
        sb.Append('}');

        int byteCount = Encoding.UTF8.GetByteCount(sb.ToString());
        if (byteCount + 1 > capacity) return -1;
        fixed (char* source = sb.ToString())
        {
            int encoded = Encoding.UTF8.GetBytes(source, sb.Length, destination, capacity - 1);
            destination[encoded] = 0;
        }
        return 0;
    }

    private static void AppendQuoted(StringBuilder sb, string value)
    {
        sb.Append('"');
        foreach (char c in value)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }
}

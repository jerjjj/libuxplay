/**
 * UxPlayInterop.cs — C# P/Invoke bindings for UxPlayLib (uxplaylib.dll).
 *
 * Usage:
 *   1. Copy uxplaylib.dll to your WinUI 3 project output directory.
 *   2. Add this file to your C# project.
 *   3. Call UxPlayLib.* static methods.
 *
 * Example:
 *   var server = UxPlayLib.Create();
 *   var config = UxPlayLib.DefaultConfig();
 *   config.ServerName = "My AirPlay";
 *   UxPlayLib.Configure(server, ref config);
 *   UxPlayLib.Start(server);
 *   // ... wait for connections ...
 *   UxPlayLib.Stop(server);
 *   UxPlayLib.Destroy(server);
 */

using System;
using System.Runtime.InteropServices;

namespace UxPlay.Interop
{
    // ============================================================================
    // Enumerations
    // ============================================================================

    public enum UxPlayState
    {
        Idle = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Error = 4
    }

    public enum UxPlayVideoFlip
    {
        None = 0,
        Left = 1,
        Right = 2,
        Invert = 3,
        VFlip = 4,
        HFlip = 5
    }

    public enum UxPlayLogLevel
    {
        Error = 3,
        Warning = 4,
        Info = 5,
        Debug = 6,
        Verbose = 7
    }

    public enum UxPlayAccessControl
    {
        Free = 0,
        Pin = 1,
        Password = 2
    }

    public enum UxPlayEventType
    {
        StateChanged = 0,
        ClientConnected = 1,
        ClientDisconnected = 2,
        DisplayPin = 3,
        MirrorStarted = 4,
        MirrorStopped = 5,
        AudioStarted = 6,
        AudioStopped = 7,
        AudioMetadata = 8,
        VideoSizeChanged = 9,
        Error = 10
    }

    public enum UxPlayError
    {
        Ok = 0,
        InvalidArgument = -1,
        AlreadyRunning = -2,
        NotRunning = -3,
        GStreamerInit = -4,
        RaopInit = -5,
        DnssdInit = -6,
        Network = -7,
        OutOfMemory = -8,
        Internal = -9
    }

    // ============================================================================
    // Structures
    // ============================================================================

    [StructLayout(LayoutKind.Sequential)]
    public struct UxPlayClientInfo
    {
        public IntPtr DeviceIdPtr;
        public IntPtr DeviceModelPtr;
        public IntPtr DeviceNamePtr;

        public string? DeviceId => Marshal.PtrToStringUTF8(DeviceIdPtr);
        public string? DeviceModel => Marshal.PtrToStringUTF8(DeviceModelPtr);
        public string? DeviceName => Marshal.PtrToStringUTF8(DeviceNamePtr);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct UxPlayAudioMeta
    {
        public IntPtr ArtistPtr;
        public IntPtr TitlePtr;
        public IntPtr AlbumPtr;

        public string? Artist => Marshal.PtrToStringUTF8(ArtistPtr);
        public string? Title => Marshal.PtrToStringUTF8(TitlePtr);
        public string? Album => Marshal.PtrToStringUTF8(AlbumPtr);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct UxPlayVideoSize
    {
        public float WidthSource;
        public float HeightSource;
        public float Width;
        public float Height;
    }

    /// <summary>
    /// Event data — use the Type field to determine which union member to read.
    /// Because the C struct uses a union, we use Explicit layout with overlapping fields.
    /// </summary>
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    public struct UxPlayEventData
    {
        [FieldOffset(0)]
        public UxPlayEventType Type;

        // Union members all start at offset 8 (after the 4-byte enum + 4-byte padding)
        [FieldOffset(8)]
        public UxPlayState State;

        [FieldOffset(8)]
        public UxPlayClientInfo Client;

        [FieldOffset(8)]
        public IntPtr PinPtr;

        [FieldOffset(8)]
        public UxPlayAudioMeta AudioMeta;

        [FieldOffset(8)]
        public UxPlayVideoSize VideoSize;

        [FieldOffset(8)]
        public IntPtr ErrorMsgPtr;

        public string? Pin => Marshal.PtrToStringUTF8(PinPtr);
        public string? ErrorMsg => Marshal.PtrToStringUTF8(ErrorMsgPtr);
    }

    /// <summary>
    /// Configuration structure. Use UxPlayLib.DefaultConfig() to get defaults,
    /// then set desired fields before calling UxPlayLib.Configure().
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct UxPlayConfig : IDisposable
    {
        // Identity
        public IntPtr ServerNamePtr;
        public IntPtr MacAddressPtr;
        [MarshalAs(UnmanagedType.U1)]
        public bool AppendHostname;

        // Display
        public ushort Width;
        public ushort Height;
        public ushort RefreshRate;
        public ushort MaxFps;

        // Video
        public IntPtr VideosinkPtr;
        public IntPtr VideosinkOptionsPtr;
        public IntPtr VideoDecoderPtr;
        public IntPtr VideoConverterPtr;
        public IntPtr VideoParserPtr;
        public UxPlayVideoFlip VideoFlip;
        [MarshalAs(UnmanagedType.U1)]
        public bool Fullscreen;
        [MarshalAs(UnmanagedType.U1)]
        public bool H265Support;
        [MarshalAs(UnmanagedType.U1)]
        public bool VideoSync;
        [MarshalAs(UnmanagedType.U1)]
        public bool Bt709Fix;
        [MarshalAs(UnmanagedType.U1)]
        public bool UseVideo;
        [MarshalAs(UnmanagedType.U1)]
        public bool NoFreeze;

        // Audio
        public IntPtr AudiosinkPtr;
        [MarshalAs(UnmanagedType.U1)]
        public bool AudioSync;
        [MarshalAs(UnmanagedType.U1)]
        public bool UseAudio;
        public double InitialVolume;
        public double DbLow;
        public double DbHigh;

        // Network
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public ushort[] TcpPorts;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public ushort[] UdpPorts;

        // Security
        public UxPlayAccessControl AccessControl;
        public IntPtr PasswordPtr;
        public IntPtr KeyfilePtr;
        [MarshalAs(UnmanagedType.U1)]
        public bool RegistrationList;

        // Misc
        public UxPlayLogLevel LogLevel;
        [MarshalAs(UnmanagedType.U1)]
        public bool CoverartDisplay;
        public IntPtr CoverartFilenamePtr;
        [MarshalAs(UnmanagedType.U1)]
        public bool HlsSupport;
        public IntPtr LangPtr;
        [MarshalAs(UnmanagedType.U1)]
        public bool NoHold;

        // ---- Managed string helpers ----
        // These allocate native memory; call Dispose() when done.

        private IntPtr _allocServerName;
        private IntPtr _allocPassword;

        public string? ServerName
        {
            get => Marshal.PtrToStringUTF8(ServerNamePtr);
            set { FreePtr(ref _allocServerName); _allocServerName = AllocUTF8(value); ServerNamePtr = _allocServerName; }
        }

        public string? Password
        {
            get => Marshal.PtrToStringUTF8(PasswordPtr);
            set { FreePtr(ref _allocPassword); var p = AllocUTF8(value); PasswordPtr = p; }
        }

        public void Dispose()
        {
            FreePtr(ref _allocServerName);
            FreePtr(ref _allocPassword);
        }

        private static IntPtr AllocUTF8(string? s)
        {
            if (s == null) return IntPtr.Zero;
            var bytes = System.Text.Encoding.UTF8.GetBytes(s + '\0');
            var ptr = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, ptr, bytes.Length);
            return ptr;
        }

        private static void FreePtr(ref IntPtr ptr)
        {
            if (ptr != IntPtr.Zero) { Marshal.FreeHGlobal(ptr); ptr = IntPtr.Zero; }
        }
    }

    // ============================================================================
    // Callback Delegates
    // ============================================================================

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void UxPlayEventCallback(IntPtr userData, ref UxPlayEventData eventData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void UxPlayLogCallback(IntPtr userData, UxPlayLogLevel level,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string message);

    // ============================================================================
    // Native API (P/Invoke)
    // ============================================================================

    public static class UxPlayLib
    {
        private const string DllName = "uxplaylib";

        // ---- Lifecycle ----

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_create(out IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_configure(IntPtr handle, ref UxPlayConfig config);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_set_event_callback(IntPtr handle,
            UxPlayEventCallback? callback, IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_set_log_callback(IntPtr handle,
            UxPlayLogCallback? callback, IntPtr userData);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_start(IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_stop(IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern void uxplay_destroy(IntPtr handle);

        // ---- Runtime Controls ----

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayState uxplay_get_state(IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_set_volume(IntPtr handle, double volume);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern int uxplay_get_connection_count(IntPtr handle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayError uxplay_disconnect_clients(IntPtr handle);

        // ---- Utility ----

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern UxPlayConfig uxplay_default_config();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr uxplay_version();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr uxplay_error_string(UxPlayError error);

        // ==================================================================
        // High-level wrappers (idiomatic C#)
        // ==================================================================

        /// <summary>Create a new UxPlay server instance.</summary>
        public static IntPtr Create()
        {
            IntPtr handle;
            var err = uxplay_create(out handle);
            if (err != UxPlayError.Ok)
                throw new InvalidOperationException($"uxplay_create failed: {GetErrorString(err)}");
            return handle;
        }

        /// <summary>Apply configuration (call before Start).</summary>
        public static void Configure(IntPtr handle, ref UxPlayConfig config)
        {
            var err = uxplay_configure(handle, ref config);
            if (err != UxPlayError.Ok)
                throw new InvalidOperationException($"uxplay_configure failed: {GetErrorString(err)}");
        }

        /// <summary>Set event callback. Hold a reference to the delegate to prevent GC.</summary>
        public static void SetEventCallback(IntPtr handle, UxPlayEventCallback? callback)
        {
            uxplay_set_event_callback(handle, callback, IntPtr.Zero);
        }

        /// <summary>Set log callback. Hold a reference to the delegate to prevent GC.</summary>
        public static void SetLogCallback(IntPtr handle, UxPlayLogCallback? callback)
        {
            uxplay_set_log_callback(handle, callback, IntPtr.Zero);
        }

        /// <summary>Start the AirPlay server (non-blocking).</summary>
        public static void Start(IntPtr handle)
        {
            var err = uxplay_start(handle);
            if (err != UxPlayError.Ok)
                throw new InvalidOperationException($"uxplay_start failed: {GetErrorString(err)}");
        }

        /// <summary>Stop the AirPlay server.</summary>
        public static void Stop(IntPtr handle)
        {
            uxplay_stop(handle);
        }

        /// <summary>Destroy the server instance and free all resources.</summary>
        public static void Destroy(IntPtr handle)
        {
            uxplay_destroy(handle);
        }

        /// <summary>Get the current server state.</summary>
        public static UxPlayState GetState(IntPtr handle) => uxplay_get_state(handle);

        /// <summary>Set the audio volume (0.0 – 1.0).</summary>
        public static void SetVolume(IntPtr handle, double volume)
        {
            uxplay_set_volume(handle, volume);
        }

        /// <summary>Get the number of connected clients.</summary>
        public static int GetConnectionCount(IntPtr handle) => uxplay_get_connection_count(handle);

        /// <summary>Disconnect all clients.</summary>
        public static void DisconnectClients(IntPtr handle) => uxplay_disconnect_clients(handle);

        /// <summary>Get a default configuration struct.</summary>
        public static UxPlayConfig DefaultConfig() => uxplay_default_config();

        /// <summary>Get the library version string.</summary>
        public static string GetVersion() => Marshal.PtrToStringUTF8(uxplay_version()) ?? "unknown";

        /// <summary>Get a human-readable error message.</summary>
        public static string GetErrorString(UxPlayError error) =>
            Marshal.PtrToStringUTF8(uxplay_error_string(error)) ?? "Unknown error";
    }
}

using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace GhostGlass.Host;
internal static partial class CredentialVault
{
    private const uint CredentialGeneric=1, PersistentLocalMachine=2;
    private const string Prefix="GhostGlass/v1/";
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]
    private struct Credential {
        public uint Flags;
        public uint Type;
        [MarshalAs(UnmanagedType.LPWStr)] public string TargetName;
        [MarshalAs(UnmanagedType.LPWStr)] public string? Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        [MarshalAs(UnmanagedType.LPWStr)] public string? TargetAlias;
        [MarshalAs(UnmanagedType.LPWStr)] public string UserName;
    }
    [DllImport("advapi32.dll",EntryPoint="CredWriteW",CharSet=CharSet.Unicode,SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredWrite(ref Credential credential,uint flags);
    [DllImport("advapi32.dll",EntryPoint="CredReadW",CharSet=CharSet.Unicode,SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredRead(string target,uint type,uint flags,out IntPtr credential);
    [DllImport("advapi32.dll",EntryPoint="CredDeleteW",CharSet=CharSet.Unicode,SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredDelete(string target,uint type,uint flags);
    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr buffer);

    private static string Target(string label){
        if(label.Length<1||label.Length>64||!ValidLabel().IsMatch(label))
            throw new ArgumentException("Credential label must be 1–64 ASCII letters, numbers, dot, hyphen or underscore.");
        return Prefix+label;
    }
    [GeneratedRegex(@"^[A-Za-z0-9_.-]{1,64}$",RegexOptions.CultureInvariant)]
    private static partial Regex ValidLabel();

    public static void Save(string label,string secret){
        if(secret.Length==0||secret.Length>4096)throw new ArgumentException("Invalid secret length.");
        byte[] bytes=Encoding.UTF8.GetBytes(secret);
        IntPtr memory=Marshal.AllocHGlobal(bytes.Length);
        try{
            Marshal.Copy(bytes,0,memory,bytes.Length);
            var c=new Credential {
                Type=CredentialGeneric,
                TargetName=Target(label),
                Comment="GhostGlass local credential reference",
                CredentialBlob=memory,
                CredentialBlobSize=(uint)bytes.Length,
                Persist=PersistentLocalMachine,
                UserName=Environment.UserName
            };
            if(!CredWrite(ref c,0))throw new IOException("Windows Credential Manager write failed: "+Marshal.GetLastWin32Error());
        }finally{
            CryptographicOperations.ZeroMemory(bytes);
            for(int i=0;i<bytes.Length;i++)Marshal.WriteByte(memory,i,0);
            Marshal.FreeHGlobal(memory);
        }
    }
    public static bool Exists(string label){
        if(!CredRead(Target(label),CredentialGeneric,0,out var ptr))return false;
        try{return true;}finally{CredFree(ptr);}
    }
    internal static string? ReadForProvider(string label){
        if(!CredRead(Target(label),CredentialGeneric,0,out var ptr))return null;
        try{
            var c=Marshal.PtrToStructure<Credential>(ptr);
            byte[] blob=new byte[c.CredentialBlobSize];
            Marshal.Copy(c.CredentialBlob,blob,0,blob.Length);
            try{return Encoding.UTF8.GetString(blob);}
            finally{CryptographicOperations.ZeroMemory(blob);}
        }finally{CredFree(ptr);}
    }
    public static bool Remove(string label) => CredDelete(Target(label),CredentialGeneric,0);
}

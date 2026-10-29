using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace GhostGlass.Host;
internal sealed class LocalIndex : IDisposable
{
    private const int SQLITE_ROW=100,SQLITE_DONE=101;
    private IntPtr db;
    private static string BaseDirectory => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"GhostGlass");
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_open_v2([MarshalAs(UnmanagedType.LPUTF8Str)] string path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_close(IntPtr db);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern IntPtr sqlite3_errmsg(IntPtr db);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_exec(IntPtr db,[MarshalAs(UnmanagedType.LPUTF8Str)] string sql,IntPtr callback,IntPtr state,out IntPtr error);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_prepare_v2(IntPtr db,[MarshalAs(UnmanagedType.LPUTF8Str)]string sql,int size,out IntPtr statement,IntPtr tail);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_bind_text(IntPtr stmt,int index,[MarshalAs(UnmanagedType.LPUTF8Str)]string value,int length,IntPtr destructor);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_step(IntPtr stmt);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern int sqlite3_finalize(IntPtr stmt);
    [DllImport("winsqlite3.dll",CallingConvention=CallingConvention.Cdecl)]
    private static extern IntPtr sqlite3_column_text(IntPtr stmt,int col);
    private static string Column(IntPtr statement,int index){
        IntPtr ptr=sqlite3_column_text(statement,index);
        return ptr==IntPtr.Zero?"":Marshal.PtrToStringUTF8(ptr)??"";
    }
    private void Check(int rc){if(rc!=0)throw new IOException("sqlite_error_"+rc);}
    public LocalIndex(){
        Directory.CreateDirectory(BaseDirectory);
        Check(sqlite3_open_v2(Path.Combine(BaseDirectory,"local-index-v1.sqlite"),out db,6|0x10000,IntPtr.Zero));
        Exec("PRAGMA busy_timeout=3000; PRAGMA journal_mode=WAL; PRAGMA secure_delete=ON;");
        Exec("CREATE TABLE IF NOT EXISTS documents (id TEXT PRIMARY KEY, path TEXT NOT NULL, content TEXT NOT NULL, added TEXT NOT NULL);");
        Exec("CREATE VIRTUAL TABLE IF NOT EXISTS documents_fts USING fts5(id UNINDEXED, content, tokenize='unicode61');");
    }
    private void Exec(string sql){Check(sqlite3_exec(db,sql,IntPtr.Zero,IntPtr.Zero,out _));}
    private T Prepare<T>(string sql,string[] args,Func<IntPtr,T> action){
        Check(sqlite3_prepare_v2(db,sql,-1,out var stmt,IntPtr.Zero));
        try{
            for(int i=0;i<args.Length;i++)Check(sqlite3_bind_text(stmt,i+1,args[i],-1,new IntPtr(-1)));
            return action(stmt);
        }finally{sqlite3_finalize(stmt);}
    }
    private void Execute(string sql,params string[] args){
        Prepare(sql,args,s=>{if(sqlite3_step(s)!=SQLITE_DONE)throw new IOException("sqlite_step_failed");return 0;});
    }
    public string Add(string selectedPath){
        string path=Path.GetFullPath(selectedPath);
        if(path.StartsWith(@"\\",StringComparison.Ordinal))throw new InvalidDataException("Network shares and device paths are not indexed.");
        string? parent=Path.GetDirectoryName(path);
        while(parent is not null){
            if((File.GetAttributes(parent)&FileAttributes.ReparsePoint)!=0)throw new InvalidDataException("Reparse-point parent directories are not accepted.");
            parent=Directory.GetParent(parent)?.FullName;
        }
        var info=new FileInfo(path);
        if(!info.Exists)throw new FileNotFoundException("File not found.");
        if((info.Attributes&FileAttributes.ReparsePoint)!=0)throw new InvalidDataException("Symbolic links are not accepted.");
        if(info.Length>1024*1024)throw new InvalidDataException("Maximum indexing file size is 1 MiB.");
        if(info.Length==0)throw new InvalidDataException("Empty files are not indexed.");
        string ext=Path.GetExtension(path).ToLowerInvariant();
        if(ext is not (".txt" or ".md" or ".json" or ".csv"))throw new InvalidDataException("Unsupported file type.");
        using var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
        if(stream.Length>1024*1024)throw new InvalidDataException("Maximum indexing file size is 1 MiB.");
        byte[] raw=new byte[(int)stream.Length];stream.ReadExactly(raw);
        string content=new UTF8Encoding(false,true).GetString(raw);
        string id=Convert.ToHexString(SHA256.HashData(raw)).ToLowerInvariant();
        Execute("INSERT OR IGNORE INTO documents(id,path,content,added) VALUES(?,?,?,?)",id,path,content,DateTimeOffset.UtcNow.ToString("O"));
        // Hash IDs deduplicate content; only one FTS row per content hash.
        var exists=Prepare("SELECT id FROM documents_fts WHERE id=?",new[]{id},s=>sqlite3_step(s)==SQLITE_ROW);
        if(!exists)Execute("INSERT INTO documents_fts(id,content) VALUES(?,?)",id,content);
        return id;
    }
    public IEnumerable<string> Search(string query){
        if(string.IsNullOrWhiteSpace(query)||query.Length>256)throw new InvalidDataException("Invalid query.");
        return Prepare("SELECT id, snippet(documents_fts,1,'[',']','...',16) FROM documents_fts WHERE documents_fts MATCH ? LIMIT 10",new[]{query},s=>{
            var results=new List<string>();while(true){int rc=sqlite3_step(s);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW)throw new InvalidDataException("Invalid search expression.");results.Add(Column(s,0)+"\t"+Column(s,1));}return results;
        });
    }
    public bool Remove(string id){
        if(id.Length!=64||id.Any(c=>!Uri.IsHexDigit(c)))throw new InvalidDataException("Invalid source ID.");
        var exists=Prepare("SELECT id FROM documents WHERE id=?",new[]{id},s=>sqlite3_step(s)==SQLITE_ROW);
        if(!exists)return false;
        Exec("BEGIN IMMEDIATE");
        try{
            Execute("DELETE FROM documents_fts WHERE id=?",id);
            Execute("DELETE FROM documents WHERE id=?",id);
            Exec("COMMIT");
            Exec("PRAGMA wal_checkpoint(TRUNCATE);");
            return true;
        }catch{Exec("ROLLBACK");throw;}
    }
    public void Dispose(){if(db!=IntPtr.Zero){sqlite3_close(db);db=IntPtr.Zero;}}
}

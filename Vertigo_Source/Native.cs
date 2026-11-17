using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Vertigo;

public static class Native
{
    [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom; public int Width=>Right-Left;public int Height=>Bottom-Top;}
    [StructLayout(LayoutKind.Sequential)] public struct Point {public int X,Y;}
    [DllImport("kernel32.dll",SetLastError=true)] public static extern nint OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)] public static extern bool ReadProcessMemory(nint handle,nint address,[Out]byte[] data,nuint size,out nuint read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(nint handle);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] public static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(nint hwnd,out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(nint hwnd,out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(nint hwnd,ref Point point);
    [DllImport("user32.dll")] public static extern bool IsWindow(nint hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(nint hwnd);
    public delegate bool EnumProc(nint hwnd,nint data);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,nint param);
    [DllImport("user32.dll",SetLastError=true)] public static extern bool SetWindowPos(nint hwnd,nint after,int x,int y,int width,int height,uint flags);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] public static extern nint GetWindowLongPtr(nint hwnd,int index);
    [DllImport("user32.dll",EntryPoint="SetWindowLongPtrW")] public static extern nint SetWindowLongPtr(nint hwnd,int index,nint value);
    [DllImport("user32.dll",SetLastError=true)] public static extern bool SetWindowDisplayAffinity(nint hwnd,uint value);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code,uint type);
    [StructLayout(LayoutKind.Sequential)] struct Mouse {public int X,Y;public uint Data,Flags,Time;public nuint Extra;}
    [StructLayout(LayoutKind.Sequential)] struct Keyboard {public ushort Vk,Scan;public uint Flags,Time;public nuint Extra;}
    [StructLayout(LayoutKind.Explicit,Size=32)] struct InputUnion {[FieldOffset(0)]public Mouse Mouse;[FieldOffset(0)]public Keyboard Key;}
    [StructLayout(LayoutKind.Sequential)] struct Input {public uint Type;public InputUnion Data;}
    [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint n,Input[] input,int size);
    static void Send(Input input) {if(SendInput(1,[input],Marshal.SizeOf<Input>())!=1) throw new Win32Exception(Marshal.GetLastWin32Error(),"SendInput отклонён");}
    public static void Move(int x,int y) {if(x!=0||y!=0)Send(new(){Data=new(){Mouse=new(){X=x,Y=y,Flags=1}}});}
    public static void MouseButton(bool down)=>Send(new(){Data=new(){Mouse=new(){Flags=down?2u:4u}}});
    public static void Key(int vk,bool down)=>Send(new(){Type=1,Data=new(){Key=new(){Scan=(ushort)MapVirtualKey((uint)vk,0),Flags=8u|(down?0u:2u)}}});
    public static bool Down(int vk)=>(GetAsyncKeyState(vk)&0x8000)!=0;
    public static bool Foreground(int pid) {GetWindowThreadProcessId(GetForegroundWindow(),out uint actual);return actual==pid;}

    public delegate nint HookProc(int code,nint w,nint l);
    [DllImport("user32.dll",SetLastError=true)] public static extern nint SetWindowsHookEx(int id,HookProc callback,nint module,uint thread);
    [DllImport("user32.dll")] public static extern nint CallNextHookEx(nint hook,int code,nint w,nint l);
    [DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(nint hook);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern nint GetModuleHandle(string? module);
}

// Track physical holds independently from our SendInput key-up events.
// No key history, text capture or persistence is performed.
public sealed class PhysicalInput:IDisposable
{
    readonly int[] keys=new int[256]; readonly Native.HookProc keyboard,mouse; readonly nint kh,mh;
    public PhysicalInput()
    {
        for(int i=1;i<256;i++)keys[i]=Native.Down(i)?1:0;
        keyboard=(code,w,l)=>{if(code>=0 && (Marshal.ReadInt32(l,8)&0x10)==0){int vk=Marshal.ReadInt32(l);if(vk is >0 and <256)Volatile.Write(ref keys[vk],w==0x100||w==0x104?1:0);}return Native.CallNextHookEx(0,code,w,l);};
        mouse=(code,w,l)=>{if(code>=0 && (Marshal.ReadInt32(l,12)&1)==0){int key=0;bool down=false;switch((int)w){case 0x201:key=1;down=true;break;case 0x202:key=1;break;case 0x204:key=2;down=true;break;case 0x205:key=2;break;case 0x207:key=4;down=true;break;case 0x208:key=4;break;case 0x20b:case 0x20c:key=((Marshal.ReadInt32(l,8)>>16)&0xffff)==1?5:6;down=w==0x20b;break;}if(key>0)Volatile.Write(ref keys[key],down?1:0);}return Native.CallNextHookEx(0,code,w,l);};
        kh=Native.SetWindowsHookEx(13,keyboard,Native.GetModuleHandle(null),0);
        mh=Native.SetWindowsHookEx(14,mouse,Native.GetModuleHandle(null),0);
        if(kh==0||mh==0){Dispose();throw new Win32Exception(Marshal.GetLastWin32Error(),"Не удалось отслеживать удержание клавиш");}
    }
    public bool Down(int key)
    {
        if(key==0x10)return Down(0xa0)||Down(0xa1);
        if(key==0x11)return Down(0xa2)||Down(0xa3);
        if(key==0x12)return Down(0xa4)||Down(0xa5);
        return key is >0 and <256 && Volatile.Read(ref keys[key])!=0;
    }
    public void Dispose(){if(kh!=0)Native.UnhookWindowsHookEx(kh);if(mh!=0)Native.UnhookWindowsHookEx(mh);}
}

public interface IGameMemory
{
    int Pid{get;}long Client{get;}long Engine{get;}
    byte[] Read(long p,int n);T Get<T>(long p)where T:unmanaged;long Ptr(long p);string Text(long p,int n=128);
    T Optional<T>(Func<T> action,T fallback);(Native.Rect,bool) Window();
}

public sealed class Memory:IDisposable,IGameMemory
{
    readonly Process process; readonly nint handle;
    public int Pid=>process.Id; public long Client{get;} public long Engine{get;}
    public Memory(Process p)
    {
        process=p;handle=Native.OpenProcess(0x1010,false,p.Id);
        if(handle==0)throw new Win32Exception(Marshal.GetLastWin32Error());
        try{foreach(ProcessModule m in p.Modules){if(m.ModuleName.Equals("client.dll",StringComparison.OrdinalIgnoreCase))Client=m.BaseAddress;
            if(m.ModuleName.Equals("engine2.dll",StringComparison.OrdinalIgnoreCase))Engine=m.BaseAddress;}
            if(Client==0||Engine==0)throw new InvalidOperationException("Ожидание client.dll / engine2.dll");
        }catch{Dispose();throw;}
    }
    public bool Alive {get{try{return !process.HasExited;}catch{return false;}}}
    public static bool Pointer(long p)=>p>=0x10000&&p<0x0000800000000000;
    public byte[] Read(long p,int n)
    {
        if(!Pointer(p)||n<=0||n>131072)throw new InvalidOperationException($"Недопустимый адрес 0x{p:X}/{n}");
        byte[] b=new byte[n];
        if(!Native.ReadProcessMemory(handle,(nint)p,b,(nuint)n,out var got)||got!=(nuint)n)
            throw new Win32Exception(Marshal.GetLastWin32Error(),$"ReadProcessMemory 0x{p:X}, {n} bytes");
        return b;
    }
    public unsafe T Get<T>(long p) where T:unmanaged {byte[] b=Read(p,sizeof(T));return MemoryMarshal.Read<T>(b);}
    public long Ptr(long p)=>Get<long>(p);
    public string Text(long p,int n=128){var b=Read(p,n);int end=Array.IndexOf(b,(byte)0);return Encoding.UTF8.GetString(b,0,end<0?n:end);}
    public T Optional<T>(Func<T> action,T fallback){try{return action();}catch(Exception e)when(e is Win32Exception or InvalidOperationException or ArgumentException){return fallback;}}
    nint window;
    public (Native.Rect,bool) Window()
    {
        if(window==0||!Native.IsWindow(window)){
            Native.EnumWindows((h,_)=>{Native.GetWindowThreadProcessId(h,out uint pid);if(pid==Pid&&Native.IsWindowVisible(h)&&Native.GetClientRect(h,out var r)&&r.Width>320&&r.Height>200){window=h;return false;}return true;},0);
        }
        if(window==0||Native.IsIconic(window)||!Native.GetClientRect(window,out var rect))return(default,false);
        var pt=new Native.Point();if(!Native.ClientToScreen(window,ref pt))return(default,false);
        rect.Right+=pt.X;rect.Bottom+=pt.Y;rect.Left=pt.X;rect.Top=pt.Y;
        return(rect,Native.GetForegroundWindow()==window);
    }
    public void Dispose(){if(handle!=0)Native.CloseHandle(handle);process.Dispose();}
}

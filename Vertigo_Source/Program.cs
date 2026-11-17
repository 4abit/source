using System.IO;
using System.Numerics;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Vertigo;

public static class Program
{
    [STAThread] public static int Main(string[] args)
    {
        bool test=args.Contains("--self-test"),demo=test||args.Contains("--demo");
        string? render=null;int at=Array.IndexOf(args,"--render");if(at>=0&&at+1<args.Length)render=Path.GetFullPath(args[at+1]);
        var app=new Application{ShutdownMode=ShutdownMode.OnExplicitShutdown};PhysicalInput? input=null;Runtime? runtime=null;OverlayWindow? overlay=null;
        app.DispatcherUnhandledException+=(_,e)=>{Log.Write(e.Exception.ToString());e.Handled=true;MessageBox.Show(e.Exception.Message,"Vertigo");app.Shutdown(1);};
        try{
            if(test)SelfTests.Run();
            var config=new Config();if(!demo){try{config=Profiles.Load("last");}catch(FileNotFoundException){}catch(DirectoryNotFoundException){}catch(Exception ex){Log.Write("Config: "+ex.Message);}}
            if(!demo)input=new PhysicalInput();
            runtime=new Runtime(config,demo,input);overlay=new OverlayWindow(runtime);var menu=new MenuWindow(runtime,overlay);
            app.MainWindow=menu;runtime.Start();
            var demoTimer=new DispatcherTimer{Interval=TimeSpan.FromMilliseconds(16)};
            if(demo){runtime.PublishDemo(Demo.Frame());demoTimer.Tick+=(_,_)=>runtime.PublishDemo(Demo.Frame());demoTimer.Start();}
            overlay.Show();menu.Show();
            if(test){
                var verify=new DispatcherTimer{Interval=TimeSpan.FromMilliseconds(800)};
                verify.Tick+=(_,_)=>{verify.Stop();try{
                    if(!menu.IsVisible||!overlay.IsVisible)throw new Exception("Windows did not show");
                    if(render!=null){Directory.CreateDirectory(render);for(int page=0;page<6;page++){menu.Page(page);menu.UpdateLayout();SaveImage(menu,Path.Combine(render,$"menu-{page}.png"));}SaveImage(overlay.Canvas,Path.Combine(render,"overlay.png"));}
                    File.WriteAllText(Path.Combine(render??AppContext.BaseDirectory,"self-test.txt"),"PASS: "+SelfTests.Count+" assertions; all six menu pages rendered; overlay rendered; no game access or input.\n");
                    app.Shutdown(0);
                }catch(Exception ex){Log.Write(ex.ToString());if(render!=null)File.WriteAllText(Path.Combine(render,"self-test.txt"),ex.ToString());app.Shutdown(1);}};verify.Start();
            }
            int result=app.Run();demoTimer.Stop();return result;
        }catch(Exception ex){Log.Write(ex.ToString());if(test&&render!=null){Directory.CreateDirectory(render);File.WriteAllText(Path.Combine(render,"self-test.txt"),ex.ToString());}else MessageBox.Show(ex.Message,"Vertigo");return 1;}
        finally{runtime?.Dispose();input?.Dispose();overlay?.Close();}
    }
    static void SaveImage(FrameworkElement visual,string path)
    {
        int w=(int)Math.Ceiling(visual.ActualWidth),h=(int)Math.Ceiling(visual.ActualHeight);if(w<=0||h<=0)throw new Exception("Zero-size visual");
        var bitmap=new RenderTargetBitmap(w,h,96,96,PixelFormats.Pbgra32);bitmap.Render(visual);var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));using var file=File.Create(path);encoder.Save(file);
    }
}

public static class Demo
{
    public static Frame Frame()
    {
        List<Player> players=[];
        for(int i=0;i<3;i++){
            var feet=new Vector3(-200+i*200,0,0);Vector3[] bones=Enumerable.Repeat(feet,28).ToArray();
            foreach(var(index,x,z)in new(int,float,float)[]{(0,0,33),(2,0,40),(4,0,49),(5,0,57),(6,0,65),(8,-8,54),(9,-16,42),(10,-18,31),(13,8,54),(14,16,42),(15,18,31),(22,-6,31),(23,-8,16),(24,-10,0),(25,6,31),(26,8,16),(27,10,0)})bones[index]=feet+new Vector3(x,0,z);
            players.Add(new(i+1,new[]{"silent","nightshift","player"}[i],new[]{87,43,100}[i],75,4200+i*900,3,feet,bones[6],bones,0,i!=1,24+i*12,false));
        }
        return new(){Active=true,Alive=true,InputReady=false,LocalTeam=2,Rect=new(){Left=30,Top=30,Right=1310,Bottom=750},
            Matrix=[.0025f,0,0,0,0,0,.017f,-.7f,0,0,1,0,0,0,0,1],Players=players.ToArray(),Nick="preview",Map="vertigo",Spectators=["observer_01","spectator"],Velocity=new(240,80,0),
            Eye=new(0,0,64),Bomb=new(new(270,0,0),"C4 / A",24.3f,7.1f),Steps=[new(new(-200,0,0),Clock.Now-.3)],Status="PREVIEW / NO GAME INPUT"};
    }
}

public static class SelfTests
{
    public static int Count{get;private set;}
    public static void Check(bool condition,string message){Count++;if(!condition)throw new Exception("TEST: "+message);}
    sealed class Output:IOutput
    {
        public List<(int,int)> Moves=[];public List<bool> Buttons=[];public List<(int,bool)> Keys=[];
        public void Move(int x,int y){if(x!=0||y!=0)Moves.Add((x,y));}public void Button(bool down)=>Buttons.Add(down);public void Key(int key,bool down)=>Keys.Add((key,down));
    }
    public static void Run()
    {
        Count=0;Check(Math.Abs(Geometry.Wrap(-358)-2)<.001,"angle wrap");
        var matrix=new float[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0};
        Check(Geometry.Project(new(0,0,-1),matrix,1920,1080)==null,"behind camera");
        Check(Geometry.Project(new(0,0,1),matrix,1920,1080)==new Vector2(960,540),"projection center");
        Check(Geometry.CleanMap("maps/de_vertigo.vpk")=="vertigo","map display");
        Check(Geometry.CleanName("a\nb\tc")=="abc","name cleanup");
        var cfg=(new Config{AimFov=double.NaN,RcsX=100,DrawFps=999,MoveKey=0x23}).Sanitize();
        Check(cfg.AimFov==3&&cfg.RcsX==1.5&&cfg.DrawFps==240&&cfg.MoveKey==32,"config bounds");
        var copy=JsonSerializer.Deserialize<Config>(JsonSerializer.Serialize(new Config{Rcs=true,Esp=false,NickOverride="тест"}));
        Check(copy is {Rcs:true,Esp:false,NickOverride:"тест"},"config roundtrip");
        try{Profiles.Save("../escape",new());throw new Exception("TEST: unsafe profile name accepted");}catch(ArgumentException){Count++;}
        var offsets=new Offsets(Path.Combine(AppContext.BaseDirectory,"Data"));
        Check(offsets.Build==14197,"bundled build");Check(offsets.G("dwBuildNumber","engine2.dll")==0x6b2d18,"verified build RVA");
        Check(offsets.F("C_CSPlayerPawn","m_iShotsFired")==7912,"new field format");
        var enemy=new Player(10,"enemy",100,0,0,3,new(1000,-20,-64),new(1000,-20,0),[],0,true,25,false);
        Frame f=new(){Time=Clock.Now,Active=true,Alive=true,InputReady=true,LocalTeam=2,Players=[enemy],Sensitivity=1,FovScale=1,Shots=1};
        Config aim=new(){Aim=true,AimFireOnly=true};
        var output=new Output();var controller=new Controller(output);
        controller.Tick(f,aim,_=>true,false,.01);Check(output.Moves.Count==0,"menu gate");
        controller.Tick(f with{Active=false},aim,_=>true,true,.01);Check(output.Moves.Count==0,"focus gate");
        controller.Tick(f with{Time=Clock.Now-1},aim,_=>true,true,.01);Check(output.Moves.Count==0,"stale sample gate");
        controller.Tick(f,aim,k=>k==1,true,.01);Check(output.Moves.Count==0,"aim bind gate");
        controller.Tick(f,aim,k=>k==2,true,.01);Check(output.Moves.Count==0,"aim fire gate");
        controller.Tick(f,aim,_=>true,true,.01);Check(output.Moves.Any(m=>m.Item1>0),"aim direction");
        output.Moves.Clear();controller.Reset(_=>false);
        controller.Tick(f with{Players=[enemy with{Team=2}]},aim,_=>true,true,.01);Check(output.Moves.Count==0,"teammate excluded");
        controller.Tick(f with{Players=[enemy with{Immune=true}]},aim,_=>true,true,.01);Check(output.Moves.Count==0,"immune excluded");
        var rcs=new Config{Rcs=true,RcsSmooth=0};
        controller.Reset(_=>false);controller.Tick(f with{Punch=new(1,0)},rcs,k=>k==1,true,.01);
        Check(output.Moves.Count==0,"first recoil sample primes baseline");
        controller.Tick(f with{Punch=new(1.3f,0),Shots=2},rcs,k=>k==1,true,.01);Check(output.Moves.Last().Item2<0,"recoil correction");
        output.Moves.Clear();controller.Tick(f with{Punch=Vector2.Zero,Shots=0},rcs,_=>false,true,.01);Check(output.Moves.Count==0,"no recoil after release");
        controller.Reset(_=>false);output.Buttons.Clear();
        var trigger=new Config{Trigger=true,TriggerDelay=0};
        controller.Tick(f with{CrosshairPawn=enemy.Address},trigger,k=>k==0x12,true,.01);Check(output.Buttons.SequenceEqual(new[]{true}),"trigger crosshair match");
        controller.Reset(_=>false);Check(output.Buttons.SequenceEqual(new[]{true,false}),"trigger release on reset");
        output.Buttons.Clear();controller.Tick(f with{CrosshairPawn=enemy.Address,Players=[enemy with{Team=2}]},trigger,k=>k==0x12,true,.01);Check(output.Buttons.Count==0,"trigger teammate gate");
        controller.Reset(_=>false);output.Keys.Clear();
        controller.Tick(f with{MoveType=2,Grounded=false,Velocity=new(200,-30,0)},new Config{Strafe=true,AutoForward=true},k=>k==32,true,.01);
        Check(output.Keys.Contains((0x41,true))&&!output.Keys.Contains((0x57,true)),"air steer without forward at speed");
        controller.Reset(_=>false);Check(output.Keys.Contains((0x41,false)),"movement cleanup");
        output.Keys.Clear();controller.Tick(f with{MoveType=9,Grounded=true},new Config{Bhop=true,AutoForward=true},k=>k==32,true,.01);Check(output.Keys.Count==0,"ladder gate");
        Check(System.Runtime.InteropServices.Marshal.SizeOf<Native.Rect>()==16,"native RECT layout");
        ReaderTests.Run(offsets);
        Log.Write($"Self-tests passed: {Count}");
    }
}

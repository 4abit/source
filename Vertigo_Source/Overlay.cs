using System.Globalization;
using System.Numerics;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace Vertigo;

public sealed class OverlayWindow:Window
{
    readonly Runtime runtime;readonly OverlayCanvas canvas;readonly DispatcherTimer timer;
    Native.Rect lastRect;bool? exclusion;public string CaptureStatus{get;private set;}="";
    public OverlayWindow(Runtime state)
    {
        runtime=state;Title="Vertigo overlay";WindowStyle=WindowStyle.None;ResizeMode=ResizeMode.NoResize;
        AllowsTransparency=true;Background=Brushes.Transparent;ShowInTaskbar=false;ShowActivated=false;Topmost=true;
        Width=1280;Height=720;Content=canvas=new OverlayCanvas(state);
        SourceInitialized+=(_,_)=>{
            var handle=new WindowInteropHelper(this).Handle;
            var style=Native.GetWindowLongPtr(handle,-20);
            Native.SetWindowLongPtr(handle,-20,(nint)((long)style|0x20|0x80|0x08000000));
        };
        timer=new DispatcherTimer(DispatcherPriority.Render){Interval=TimeSpan.FromMilliseconds(8)};
        timer.Tick+=(_,_)=>Tick();timer.Start();Closed+=(_,_)=>timer.Stop();
    }
    public OverlayCanvas Canvas=>canvas;
    void Tick()
    {
        var f=runtime.Frame;var c=runtime.Config;timer.Interval=TimeSpan.FromMilliseconds(1000.0/c.DrawFps);
        if(c.CaptureExclude!=exclusion){
            var h=new WindowInteropHelper(this).Handle;
            if(h!=0){bool ok=Native.SetWindowDisplayAffinity(h,c.CaptureExclude?0x11u:0u);CaptureStatus=ok?(c.CaptureExclude?"Исключение захвата включено":"Захват разрешён"):"Windows отклонила исключение захвата";exclusion=c.CaptureExclude;}
        }
        if(!f.Active||Clock.Now-f.Time>.3){if(IsVisible)Hide();return;}
        if(!IsVisible)Show();
        if(!lastRect.Equals(f.Rect)){
            var r=f.Rect;Native.SetWindowPos(new WindowInteropHelper(this).Handle,(nint)(-1),r.Left,r.Top,r.Width,r.Height,0x10|0x200);
            lastRect=r;
        }
        canvas.InvalidateVisual();
    }
}

public sealed class OverlayCanvas(Runtime runtime):FrameworkElement
{
    readonly Typeface face=new("Segoe UI");double fpsAt=Clock.Now;int frames;public double Fps{get;private set;}
    static SolidColorBrush Brush(byte shade,double alpha=1){var b=new SolidColorBrush(Color.FromArgb((byte)(Math.Clamp(alpha,0,1)*255),shade,shade,shade));b.Freeze();return b;}
    void Text(DrawingContext dc,string value,double x,double y,double size=12,Brush? color=null,bool shadow=true)
    {
        var ft=new FormattedText(value,CultureInfo.InvariantCulture,FlowDirection.LeftToRight,face,size,color??Brushes.White,1);
        if(shadow){dc.DrawRectangle(Brush(0,.65),null,new Rect(x-3,y-1,ft.Width+6,ft.Height+2));}
        dc.DrawText(ft,new Point(x,y));
    }
    static void Line(DrawingContext dc,Vector2 a,Vector2 b,double width,Brush brush,bool outline=true)
    {if(outline)dc.DrawLine(new Pen(Brush(0,.8),width+2),new(a.X,a.Y),new(b.X,b.Y));dc.DrawLine(new Pen(brush,width),new(a.X,a.Y),new(b.X,b.Y));}
    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);var f=runtime.Frame;var c=runtime.Config;if(!f.Active)return;
        frames++;if(Clock.Now-fpsAt>=.5){Fps=frames/(Clock.Now-fpsAt);frames=0;fpsAt=Clock.Now;}
        var dpi=VisualTreeHelper.GetDpi(this);dc.PushTransform(new ScaleTransform(1/dpi.DpiScaleX,1/dpi.DpiScaleY));
        double w=f.Rect.Width,h=f.Rect.Height;dc.PushClip(new RectangleGeometry(new Rect(0,0,w,h)));
        Vector2? Project(Vector3 pos)=>Geometry.Project(pos,f.Matrix,w,h);
        if(c.Esp){foreach(var enemy in f.Players){
            var top=Project(enemy.Head+new Vector3(0,0,7));var bottom=Project(enemy.Feet-new Vector3(0,0,2));if(top==null||bottom==null)continue;
            double bh=bottom.Value.Y-top.Value.Y,bw=bh*.44,x=top.Value.X-bw/2,y=top.Value.Y;
            if(bh<5||bh>h*2||x+bw<0||x>w||y+bh<0||y>h)continue;
            var color=Brush(enemy.Spotted?(byte)255:(byte)180,c.EspOpacity);
            if(c.Boxes){
                var a=new Vector2((float)x,(float)y);var b=new Vector2((float)(x+bw),(float)(y+bh));
                if(c.Corners){float len=(float)Math.Min(bw*.28,25);foreach(var(xx,yy,sx,sy) in new[]{(a.X,a.Y,1,1),(b.X,a.Y,-1,1),(a.X,b.Y,1,-1),(b.X,b.Y,-1,-1)}){Line(dc,new(xx,yy),new(xx+sx*len,yy),c.LineWidth,color);Line(dc,new(xx,yy),new(xx,yy+sy*len),c.LineWidth,color);}}
                else{dc.DrawRectangle(null,new Pen(Brushes.Black,c.LineWidth+2),new Rect(x,y,bw,bh));dc.DrawRectangle(null,new Pen(color,c.LineWidth),new Rect(x,y,bw,bh));}
            }
            if(c.Skeleton&&enemy.Bones.Length>=28){foreach(var(a,b)in Geometry.Links[enemy.Rig]){
                var ba=enemy.Bones[a];var bb=enemy.Bones[b];if(!Geometry.Valid(ba)||!Geometry.Valid(bb)||Vector3.Distance(ba,bb)>60)continue;
                var pa=Project(ba);var pb=Project(bb);if(pa!=null&&pb!=null&&Vector2.Distance(pa.Value,pb.Value)<bh)Line(dc,pa.Value,pb.Value,c.LineWidth,color);
            }}
            if(c.HeadDot&&Project(enemy.Head) is Vector2 head)dc.DrawEllipse(null,new Pen(color,c.LineWidth),new(head.X,head.Y),Math.Max(2,bh*.025),Math.Max(2,bh*.025));
            if(c.Health){dc.DrawRectangle(Brushes.Black,null,new Rect(x-8,y-1,5,bh+2));double hp=bh*Math.Clamp(enemy.Hp/100.0,0,1);dc.DrawRectangle(Brushes.White,null,new Rect(x-7,y+bh-hp,3,hp));if(enemy.Hp<100)Text(dc,enemy.Hp.ToString(),x-31,y+bh-hp-6,10);}
            if(c.Armor){dc.DrawRectangle(Brushes.Black,null,new Rect(x,y+bh+3,bw,3));dc.DrawRectangle(Brush(150),null,new Rect(x,y+bh+3,bw*Math.Clamp(enemy.Armor/100.0,0,1),3));}
            if(c.Names){var name=enemy.Name;Text(dc,name,x+bw/2-name.Length*3.1,y-19,12);}
            List<string> labels=[];if(c.Money)labels.Add(enemy.Money.HasValue?$"${enemy.Money}":"$?");if(c.Distance)labels.Add($"{enemy.Distance:0} m");
            if(labels.Count>0)Text(dc,string.Join("  /  ",labels),x,y+bh+(c.Armor?10:5),11);
        }}
        if(c.Steps&&c.Esp){foreach(var step in f.Steps){double age=Clock.Now-step.Time;if(age<0||age>1.2)continue;double radius=10+age*35;var brush=Brush(255,(1-age/1.2)*.8);
            Vector2? previous=null;for(int i=0;i<=40;i++){double a=i*Math.PI*2/40;var point=Project(step.Position+new Vector3((float)(Math.Cos(a)*radius),(float)(Math.Sin(a)*radius),2));if(previous!=null&&point!=null)Line(dc,previous.Value,point.Value,1.2,brush,false);previous=point;}
        }}
        if(c.Aim&&c.FovCircle){double radius=Math.Tan(c.AimFov*Math.PI/180)/Math.Tan(f.HFov*Math.PI/360)*w/2;dc.DrawEllipse(null,new Pen(Brush(255,.3),1),new(w/2,h/2),radius,radius);}
        if(c.Watermark){
            List<string> parts=["vertigo"];
            if(c.WatermarkNick)parts.Add(string.IsNullOrWhiteSpace(c.NickOverride)?f.Nick:c.NickOverride);
            if(c.WatermarkFps)parts.Add($"{Fps:0} fps");if(c.WatermarkMap&&!string.IsNullOrEmpty(f.Map))parts.Add(f.Map);
            if(c.WatermarkClock)parts.Add(DateTime.Now.ToString("HH:mm"));
            string label=string.Join("   /   ",parts);double width=Math.Min(w-36,Math.Max(260,label.Length*7+30));double x=w-width-18;
            dc.DrawRectangle(Brush(8,.93),new Pen(Brush(80),1),new Rect(x,18,width,34));dc.DrawRectangle(Brushes.White,null,new Rect(x,18,3,34));Text(dc,label,x+14,26,12,Brushes.White,false);
        }
        if(c.Spectators){double x=w-238,y=c.Watermark?64:18;double height=38+Math.Max(1,f.Spectators.Length)*22;
            dc.DrawRectangle(Brush(8,.85),new Pen(Brush(60),1),new Rect(x,y,220,height));Text(dc,$"SPECTATORS   {f.Spectators.Length:00}",x+12,y+10,11,Brushes.White,false);
            dc.DrawLine(new Pen(Brush(50),1),new(x+12,y+31),new(x+208,y+31));
            if(f.Spectators.Length==0)Text(dc,"никого",x+12,y+39,11,Brush(130),false);
            else for(int i=0;i<f.Spectators.Length;i++)Text(dc,f.Spectators[i],x+12,y+39+i*22,12,Brushes.White,false);
        }
        if(c.Bomb&&f.Bomb is BombInfo bomb){
            string detail=bomb.Defused?"DEFUSED":bomb.Remaining.HasValue?$"{Math.Max(0,bomb.Remaining.Value):0.0}s":"";
            if(bomb.DefuseRemaining.HasValue)detail+=$"   /   DEFUSE {bomb.DefuseRemaining:0.0}s";
            double distance=Vector3.Distance(f.Eye,bomb.Position)*.0254;
            Text(dc,$"{bomb.Label}   {detail}",w/2-120,24,14);
            if(Project(bomb.Position) is Vector2 bp&&bp.X>=20&&bp.X<w-130&&bp.Y>=60&&bp.Y<h-20){
                dc.DrawRectangle(Brush(0,.8),new Pen(Brushes.White,1),new Rect(bp.X-4,bp.Y-4,8,8));Text(dc,$"{bomb.Label}  /  {distance:0} m",bp.X+12,bp.Y-8,12);
            }else{
                double yaw=Geometry.Wrap(Geometry.Look(f.Eye,bomb.Position).Y-f.Angles.Y)*Math.PI/180;
                var center=new Vector2((float)(w/2-Math.Sin(yaw)*130),(float)(h/2-Math.Cos(yaw)*130));
                Text(dc,$"C4  /  {distance:0} m",center.X,center.Y,12);
            }
        }
        if(c.Speedometer){double speed=new Vector2(f.Velocity.X,f.Velocity.Y).Length();Text(dc,$"{speed:0} u/s",w/2-25,h-100,20);}
        if(runtime.Demo)Text(dc,"PREVIEW / synthetic scene / no game input",20,h-32,12,Brush(180));
        dc.Pop();dc.Pop();
    }
}

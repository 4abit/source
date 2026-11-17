using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Threading;

namespace Vertigo;

public sealed class MenuWindow:Window
{
    readonly Runtime runtime;readonly OverlayWindow overlay;readonly Grid body=new();readonly TextBlock status=new(),notice=new();
    readonly List<Button> tabs=[];readonly DispatcherTimer timer;int current;bool insertDown,endDown;bool? capture;
    string? binding;Button? bindingButton;double bindStarted;
    public MenuWindow(Runtime state,OverlayWindow overlayWindow)
    {
        runtime=state;overlay=overlayWindow;Title="vertigo";Width=920;Height=710;MinWidth=820;MinHeight=650;
        WindowStyle=WindowStyle.None;ResizeMode=ResizeMode.NoResize;Background=B(8);Foreground=Brushes.White;FontFamily=new FontFamily("Segoe UI");FontSize=13;Topmost=true;
        WindowStartupLocation=WindowStartupLocation.CenterScreen;
        Resources=(ResourceDictionary)XamlReader.Parse(Styles);
        var border=new Border{BorderBrush=B(65),BorderThickness=new Thickness(1),Padding=new Thickness(25,20,25,16)};Content=border;
        var root=new Grid();border.Child=root;
        foreach(var length in new[]{GridLength.Auto,GridLength.Auto,new GridLength(1,GridUnitType.Star),GridLength.Auto,GridLength.Auto})root.RowDefinitions.Add(new(){Height=length});
        var title=new DockPanel{Margin=new Thickness(0,0,0,22)};title.MouseLeftButtonDown+=(_,e)=>{if(e.OriginalSource is TextBlock||e.OriginalSource==title)DragMove();};
        var close=Button("×",()=>Application.Current.Shutdown());close.Width=32;close.FontSize=20;close.Padding=new Thickness(0);DockPanel.SetDock(close,Dock.Right);title.Children.Add(close);
        var hide=Button("—",HideMenu);hide.Width=32;hide.Padding=new Thickness(0);DockPanel.SetDock(hide,Dock.Right);title.Children.Add(hide);
        var mark=new StackPanel{Orientation=Orientation.Horizontal};mark.Children.Add(new TextBlock{Text="vertigo",FontSize=32,FontWeight=FontWeights.Bold,VerticalAlignment=VerticalAlignment.Center});
        mark.Children.Add(new TextBlock{Text="/  EXTERNAL",FontSize=10,Foreground=B(125),Margin=new Thickness(15,13,0,0)});title.Children.Add(mark);root.Children.Add(title);
        var navigation=new UniformGridEx(6){Margin=new Thickness(0,0,0,23)};Grid.SetRow(navigation,1);root.Children.Add(navigation);
        string[] titles=["AIM / RCS","VISUALS","WORLD","MOVEMENT","INTERFACE","CONFIGS"];
        for(int i=0;i<titles.Length;i++){int index=i;var b=Button(titles[i],()=>Page(index));b.FontSize=11;b.Padding=new Thickness(5,12,5,12);b.Margin=new Thickness(0,0,i<5?5:0,0);tabs.Add(b);navigation.Children.Add(b);}
        Grid.SetRow(body,2);root.Children.Add(body);
        notice.Foreground=B(170);notice.FontSize=11;notice.Margin=new Thickness(0,8,0,8);notice.TextWrapping=TextWrapping.Wrap;Grid.SetRow(notice,3);root.Children.Add(notice);
        var footer=new DockPanel{LastChildFill=true};Grid.SetRow(footer,4);root.Children.Add(footer);
        var hint=new TextBlock{Text="INSERT  MENU     END  EXIT",FontSize=10,Foreground=B(125),VerticalAlignment=VerticalAlignment.Center};DockPanel.SetDock(hint,Dock.Right);footer.Children.Add(hint);
        status.FontSize=10;status.Foreground=B(155);status.TextTrimming=TextTrimming.CharacterEllipsis;status.Margin=new Thickness(0,0,15,0);status.VerticalAlignment=VerticalAlignment.Center;footer.Children.Add(status);
        Page(0);
        timer=new DispatcherTimer{Interval=TimeSpan.FromMilliseconds(25)};timer.Tick+=(_,_)=>Tick();timer.Start();
        Closing+=(_,_)=>{timer.Stop();if(!runtime.Demo)try{Profiles.Save("last",runtime.Config);}catch(Exception ex){Log.Write(ex.Message);}};
        Closed+=(_,_)=>Application.Current.Shutdown();
    }
    public int PageIndex=>current;
    public void Page(int index)
    {
        CancelBind();current=index;body.Children.Clear();body.ColumnDefinitions.Clear();
        body.ColumnDefinitions.Add(new(){Width=new GridLength(1,GridUnitType.Star)});body.ColumnDefinitions.Add(new(){Width=new GridLength(22)});body.ColumnDefinitions.Add(new(){Width=new GridLength(1,GridUnitType.Star)});
        for(int i=0;i<tabs.Count;i++){tabs[i].Background=i==index?Brushes.White:B(15);tabs[i].Foreground=i==index?Brushes.Black:B(160);}
        var left=new StackPanel();var right=new StackPanel();
        var sl=new ScrollViewer{Content=left,VerticalScrollBarVisibility=ScrollBarVisibility.Auto,HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled};
        var sr=new ScrollViewer{Content=right,VerticalScrollBarVisibility=ScrollBarVisibility.Auto,HorizontalScrollBarVisibility=ScrollBarVisibility.Disabled};
        Grid.SetColumn(sr,2);body.Children.Add(sl);body.Children.Add(sr);
        if(index==0){
            var a=Card(left,"01 / AIM ASSIST");Toggle(a,"Наведение",nameof(Config.Aim));Bind(a,"Удерживать",nameof(Config.AimKey));
            Toggle(a,"Только при стрельбе",nameof(Config.AimFireOnly));Toggle(a,"Только отмеченные",nameof(Config.SpottedOnly));
            Choice(a,"Точка",nameof(Config.AimBone),["Голова","Шея","Грудь"]);
            Slider(a,"Угол FOV",nameof(Config.AimFov),.2,15,"0.0°");Slider(a,"Плавность",nameof(Config.AimSmooth),1,500,"0 ms");Toggle(a,"Круг FOV",nameof(Config.FovCircle));
            var r=Card(right,"02 / RECOIL CONTROL");Toggle(r,"Контроль отдачи",nameof(Config.Rcs));
            Slider(r,"Горизонталь",nameof(Config.RcsX),0,1.5,"0.00×");Slider(r,"Вертикаль",nameof(Config.RcsY),0,1.5,"0.00×");
            Slider(r,"Плавность RCS",nameof(Config.RcsSmooth),0,100,"0 ms");Slider(r,"Начать с выстрела",nameof(Config.RcsStart),1,10,"0");
            Note(r,"RCS работает при стрельбе. Плавность сохраняет остаток коррекции между кадрами.");
            var t=Card(right,"03 / TRIGGER");Toggle(t,"Автовыстрел под прицелом",nameof(Config.Trigger));Bind(t,"Удерживать",nameof(Config.TriggerKey));
            Slider(t,"Задержка",nameof(Config.TriggerDelay),0,500,"0 ms");Slider(t,"Интервал",nameof(Config.TriggerInterval),50,1000,"0 ms");Toggle(t,"Только отмеченные",nameof(Config.TriggerSpotted));Toggle(t,"Только при малой скорости",nameof(Config.TriggerStationary));
        }else if(index==1){
            var a=Card(left,"01 / PLAYER ESP");Toggle(a,"Включить ESP",nameof(Config.Esp));Toggle(a,"Рамки",nameof(Config.Boxes));Toggle(a,"Угловые рамки",nameof(Config.Corners));Toggle(a,"Скелет",nameof(Config.Skeleton));Toggle(a,"Контур головы",nameof(Config.HeadDot));Toggle(a,"Имена",nameof(Config.Names));Toggle(a,"Здоровье",nameof(Config.Health));Toggle(a,"Броня",nameof(Config.Armor));Toggle(a,"Деньги",nameof(Config.Money));Toggle(a,"Дистанция",nameof(Config.Distance));Toggle(a,"Союзники",nameof(Config.Teammates));
            var p=Card(right,"02 / LIVE PREVIEW");var preview=new EspPreview(runtime){Height=195};p.Children.Add(preview);
            var s=Card(right,"03 / STYLE");Slider(s,"Толщина",nameof(Config.LineWidth),.5,3,"0.0 px");Slider(s,"Непрозрачность",nameof(Config.EspOpacity),.2,1,"0%");Slider(s,"Дистанция ESP",nameof(Config.EspRange),10,500,"0 m");Choice(s,"Скелет модели",nameof(Config.Rig),["Авто","Стандартный","Альтернативный"]);
        }else if(index==2){
            var a=Card(left,"01 / WORLD");Toggle(a,"Бомба: позиция и таймер",nameof(Config.Bomb));Toggle(a,"Метки шагов",nameof(Config.Steps));
            Note(a,"Метка шага затухает за 1.2 секунды. Для неё нужны доступные данные движения противника.");
            var b=Card(left,"02 / SPECTATORS");Toggle(b,"Список наблюдателей",nameof(Config.Spectators));Note(b,"Показывает клиентов, чья цель наблюдения — твой персонаж.");
            var c=Card(right,"03 / ON SCREEN");Note(c,"C4: установленная бомба, таймер взрыва и разминирования. Для переносимой или брошенной — позиция и расстояние.");Note(c,"Деньги и наблюдатели отображаются только когда эти данные доступны клиенту. Неизвестные деньги отмечены как $?.");
            var st=Card(right,"04 / STATUS");Note(st,"Белый — отмеченный игрок. Серый — не отмеченный. Отметка не гарантирует прямую видимость и не проверяет дым.");
        }else if(index==3){
            var a=Card(left,"01 / MOVEMENT");Toggle(a,"Bunny hop",nameof(Config.Bhop));Toggle(a,"Стрейф по направлению взгляда",nameof(Config.Strafe));Toggle(a,"Автоматически идти вперёд",nameof(Config.AutoForward));Bind(a,"Удерживать",nameof(Config.MoveKey));Toggle(a,"Индикатор скорости",nameof(Config.Speedometer));
            Note(a,"По умолчанию — удержание Space. Движение останавливается при открытии меню, смерти или потере фокуса.");
            var b=Card(right,"02 / CONTROLS");Note(b,"Используются стандартные бинды CS2: Space — прыжок; W / A / D — движение. Верни эти бинды, если изменял их.");Note(b,"Стрейф подруливает в воздухе в сторону взгляда. Разгон и частота прыжков зависят от физики и настроек сервера.");
        }else if(index==4){
            var a=Card(left,"01 / WATERMARK");Toggle(a,"Ватермарка vertigo",nameof(Config.Watermark));Toggle(a,"Ник",nameof(Config.WatermarkNick));Toggle(a,"FPS оверлея",nameof(Config.WatermarkFps));Toggle(a,"Часы",nameof(Config.WatermarkClock));Toggle(a,"Карта без префикса",nameof(Config.WatermarkMap));
            Label(a,"Свой ник · пусто = ник из игры");var nick=new TextBox{Text=runtime.Config.NickOverride,MaxLength=32,Margin=new Thickness(0,4,0,8)};nick.TextChanged+=(_,_)=>Set(nameof(Config.NickOverride),nick.Text);a.Children.Add(nick);
            var b=Card(right,"02 / APPLICATION");Toggle(b,"Исключать из захвата OBS / Windows",nameof(Config.CaptureExclude));Note(b,"Исключение применяется к меню и оверлею. Проверь запись: поддержка зависит от метода захвата.");
            Slider(b,"Частота отрисовки",nameof(Config.DrawFps),30,240,"0 FPS");
            var m=Card(right,"03 / MOUSE CALIBRATION");Slider(m,"m_yaw",nameof(Config.MouseYaw),.001,.05,"0.000");Slider(m,"m_pitch",nameof(Config.MousePitch),.001,.05,"0.000");Note(m,"Стандартные значения — 0.022. Чувствительность и масштаб прицела читаются из игры.");
        }else{
            var a=Card(left,"01 / PROFILES");Label(a,"Имя профиля");var name=new TextBox{Text="default",Margin=new Thickness(0,6,0,12)};a.Children.Add(name);
            var list=new ComboBox{ItemsSource=Profiles.List(),Margin=new Thickness(0,0,0,12),MinHeight=34};list.SelectionChanged+=(_,_)=>{if(list.SelectedItem is string s)name.Text=s;};a.Children.Add(list);
            a.Children.Add(Button("СОХРАНИТЬ",()=>Try(()=>{Profiles.Save(name.Text,runtime.Config);list.ItemsSource=Profiles.List();notice.Text="Профиль сохранён: "+name.Text;})));
            a.Children.Add(Button("ЗАГРУЗИТЬ",()=>Try(()=>{runtime.Config=Profiles.Load(name.Text);notice.Text="Профиль загружен: "+name.Text;Page(5);})));
            a.Children.Add(Button("СБРОСИТЬ НАСТРОЙКИ",()=>{runtime.Config=new();notice.Text="Настройки сброшены. Сохранённые профили остались.";Page(5);}));
            var b=Card(right,"02 / FILES");Note(b,"Последние настройки сохраняются автоматически при закрытии. Конфиги и журнал лежат отдельно от приложения.");
            b.Children.Add(Button("ОТКРЫТЬ ПАПКУ КОНФИГОВ",()=>Try(()=>{Directory.CreateDirectory(Profiles.DirectoryPath);Process.Start(new ProcessStartInfo("explorer.exe",Profiles.DirectoryPath){UseShellExecute=true});})));
            b.Children.Add(Button("ОТКРЫТЬ ЖУРНАЛ",()=>Try(()=>{Log.Write("Log opened");Process.Start(new ProcessStartInfo("notepad.exe",Path.Combine(Profiles.Root,"vertigo.log")){UseShellExecute=true});})));
            b.Children.Add(Button("СОХРАНИТЬ И СКРЫТЬ",()=>Try(()=>{Profiles.Save("last",runtime.Config);HideMenu();})));
        }
    }
    void Set(string property,object value)
    {
        var cfg=runtime.Config with{};typeof(Config).GetProperty(property)!.SetValue(cfg,value);runtime.Config=cfg;
    }
    void Try(Action action){try{action();}catch(Exception e){notice.Text=e.Message;Log.Write(e.ToString());}}
    void Tick()
    {
        bool ins=Native.Down(0x2d),end=Native.Down(0x23);
        if(ins&&!insertDown){if(IsVisible)HideMenu();else{runtime.MenuOpen=true;Show();Activate();}}
        if(end&&!endDown)Application.Current.Shutdown();insertDown=ins;endDown=end;
        status.Text=runtime.Frame.Status;status.ToolTip=runtime.Frame.Status+"\n"+overlay.CaptureStatus;
        if(binding!=null&&Clock.Now-bindStarted>.25){for(int k=1;k<255;k++)if(k!=0x2d&&k!=0x23&&Native.Down(k)){if(k==0x1b){CancelBind();break;}Set(binding,k);CancelBind();break;}}
        var c=runtime.Config;
        if(c.CaptureExclude!=capture){var h=new WindowInteropHelper(this).Handle;if(h!=0){bool ok=Native.SetWindowDisplayAffinity(h,c.CaptureExclude?0x11u:0u);capture=c.CaptureExclude;if(!ok)notice.Text="Windows отклонила исключение меню из захвата.";}}
    }
    void HideMenu(){CancelBind();runtime.MenuOpen=false;Hide();}
    void CancelBind(){if(binding!=null&&bindingButton!=null)bindingButton.Content=KeyName((int)typeof(Config).GetProperty(binding)!.GetValue(runtime.Config)!);binding=null;bindingButton=null;}
    public static string KeyName(int vk)=>vk switch{1=>"MOUSE 1",2=>"MOUSE 2",4=>"MOUSE 3",5=>"MOUSE 4",6=>"MOUSE 5",0x20=>"SPACE",0x12 or 0xa4 or 0xa5=>"ALT",0x10 or 0xa0 or 0xa1=>"SHIFT",0x11 or 0xa2 or 0xa3=>"CTRL",_=>KeyInterop.KeyFromVirtualKey(vk).ToString().ToUpperInvariant()};
    void Bind(Panel p,string text,string property)
    {
        var row=new DockPanel{Margin=new Thickness(0,7,0,7)};var b=Button(KeyName((int)typeof(Config).GetProperty(property)!.GetValue(runtime.Config)!),()=>{});
        b.Width=110;b.FontSize=11;b.Click+=(_,_)=>{CancelBind();binding=property;bindingButton=b;bindStarted=Clock.Now;b.Content="НАЖМИ / ESC";};DockPanel.SetDock(b,Dock.Right);row.Children.Add(b);row.Children.Add(new TextBlock{Text=text,VerticalAlignment=VerticalAlignment.Center,Foreground=B(190)});p.Children.Add(row);
    }
    void Toggle(Panel p,string text,string property)
    {
        var box=new CheckBox{Content=text,IsChecked=(bool)typeof(Config).GetProperty(property)!.GetValue(runtime.Config)!,Margin=new Thickness(0,6,0,6)};
        box.Checked+=(_,_)=>Set(property,true);box.Unchecked+=(_,_)=>Set(property,false);p.Children.Add(box);
    }
    void Slider(Panel p,string text,string property,double min,double max,string format)
    {
        var info=typeof(Config).GetProperty(property)!;double value=Convert.ToDouble(info.GetValue(runtime.Config));
        var row=new DockPanel{Margin=new Thickness(0,9,0,0)};var label=new TextBlock{Text=value.ToString(format),Foreground=Brushes.White,FontSize=11};DockPanel.SetDock(label,Dock.Right);row.Children.Add(label);row.Children.Add(new TextBlock{Text=text,Foreground=B(170),FontSize=12});p.Children.Add(row);
        var slider=new Slider{Minimum=min,Maximum=max,Value=value,Margin=new Thickness(0,4,0,4),Height=19,SmallChange=(max-min)/100,IsSnapToTickEnabled=info.PropertyType==typeof(int),TickFrequency=1};
        slider.ValueChanged+=(_,_)=>{double v=slider.Value;label.Text=v.ToString(format);Set(property,info.PropertyType==typeof(int)?(object)(int)Math.Round(v):v);};p.Children.Add(slider);
    }
    void Choice(Panel p,string text,string property,string[] choices)
    {Label(p,text);var cb=new ComboBox{ItemsSource=choices,SelectedIndex=(int)typeof(Config).GetProperty(property)!.GetValue(runtime.Config)!,Margin=new Thickness(0,5,0,6)};cb.SelectionChanged+=(_,_)=>{if(cb.SelectedIndex>=0)Set(property,cb.SelectedIndex);};p.Children.Add(cb);}
    static void Label(Panel p,string text)=>p.Children.Add(new TextBlock{Text=text,Foreground=B(170),FontSize=12,Margin=new Thickness(0,7,0,0)});
    static void Note(Panel p,string text)=>p.Children.Add(new TextBlock{Text=text,Foreground=B(128),FontSize=11,TextWrapping=TextWrapping.Wrap,LineHeight=18,Margin=new Thickness(0,8,0,8)});
    static StackPanel Card(Panel parent,string title)
    {var border=new Border{BorderBrush=B(39),BorderThickness=new Thickness(1),Background=B(12),Padding=new Thickness(17,14,17,12),Margin=new Thickness(0,0,0,14)};var p=new StackPanel();border.Child=p;parent.Children.Add(border);p.Children.Add(new TextBlock{Text=title,Foreground=B(230),FontWeight=FontWeights.SemiBold,FontSize=11,Margin=new Thickness(0,0,0,12)});return p;}
    static Button Button(string title,Action click){var b=new Button{Content=title,Padding=new Thickness(12,8,12,8),Margin=new Thickness(0,0,0,5)};b.Click+=(_,_)=>click();return b;}
    static SolidColorBrush B(byte value)=>new(Color.FromRgb(value,value,value));
    const string Styles="""
    <ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
      <Style TargetType="Button"><Setter Property="Background" Value="#171717"/><Setter Property="Foreground" Value="White"/><Setter Property="BorderBrush" Value="#3A3A3A"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button"><Border x:Name="b" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" Padding="{TemplateBinding Padding}"><ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border><ControlTemplate.Triggers><Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="White"/></Trigger><Trigger Property="IsPressed" Value="True"><Setter TargetName="b" Property="Opacity" Value="0.65"/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="CheckBox"><Setter Property="Foreground" Value="#DDDDDD"/><Setter Property="Cursor" Value="Hand"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="CheckBox"><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="22"/></Grid.ColumnDefinitions><ContentPresenter VerticalAlignment="Center"/><Border x:Name="b" Grid.Column="1" Width="16" Height="16" Background="#151515" BorderBrush="#555555" BorderThickness="1"><TextBlock x:Name="check" Text="✓" FontSize="12" Foreground="Black" HorizontalAlignment="Center" VerticalAlignment="Center" Visibility="Collapsed"/></Border></Grid><ControlTemplate.Triggers><Trigger Property="IsChecked" Value="True"><Setter TargetName="b" Property="Background" Value="White"/><Setter TargetName="b" Property="BorderBrush" Value="White"/><Setter TargetName="check" Property="Visibility" Value="Visible"/></Trigger><Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="White"/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="TextBox"><Setter Property="Foreground" Value="White"/><Setter Property="CaretBrush" Value="White"/><Setter Property="Background" Value="#171717"/><Setter Property="BorderBrush" Value="#444444"/><Setter Property="Padding" Value="8"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="TextBox"><Border Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1"><ScrollViewer x:Name="PART_ContentHost" Margin="{TemplateBinding Padding}"/></Border></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="ComboBox"><Setter Property="Foreground" Value="White"/><Setter Property="Background" Value="#171717"/><Setter Property="BorderBrush" Value="#444444"/><Setter Property="Padding" Value="8"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ComboBox"><Grid><ToggleButton Focusable="False" IsChecked="{Binding IsDropDownOpen, RelativeSource={RelativeSource TemplatedParent}, Mode=TwoWay}"><ToggleButton.Template><ControlTemplate TargetType="ToggleButton"><Border Background="#171717" BorderBrush="#444444" BorderThickness="1"><TextBlock Text="⌄" Foreground="White" HorizontalAlignment="Right" Margin="0,5,8,5"/></Border></ControlTemplate></ToggleButton.Template></ToggleButton><ContentPresenter Content="{TemplateBinding SelectionBoxItem}" Margin="8,7,28,7" IsHitTestVisible="False"/><Popup x:Name="PART_Popup" IsOpen="{TemplateBinding IsDropDownOpen}" Placement="Bottom" AllowsTransparency="True"><Border Background="#171717" BorderBrush="#555555" BorderThickness="1" MinWidth="180"><ScrollViewer MaxHeight="240"><ItemsPresenter/></ScrollViewer></Border></Popup></Grid></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="ComboBoxItem"><Setter Property="Foreground" Value="White"/><Setter Property="Padding" Value="8"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ComboBoxItem"><Border x:Name="b" Background="#171717" Padding="{TemplateBinding Padding}"><ContentPresenter/></Border><ControlTemplate.Triggers><Trigger Property="IsHighlighted" Value="True"><Setter TargetName="b" Property="Background" Value="#444444"/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="Slider"><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Slider"><Grid VerticalAlignment="Center"><Border Height="2" Background="#404040"/><Track x:Name="PART_Track"><Track.DecreaseRepeatButton><RepeatButton Command="Slider.DecreaseLarge" Opacity="0"/></Track.DecreaseRepeatButton><Track.IncreaseRepeatButton><RepeatButton Command="Slider.IncreaseLarge" Opacity="0"/></Track.IncreaseRepeatButton><Track.Thumb><Thumb Width="7" Height="13"><Thumb.Template><ControlTemplate TargetType="Thumb"><Border Background="White"/></ControlTemplate></Thumb.Template></Thumb></Track.Thumb></Track></Grid></ControlTemplate></Setter.Value></Setter></Style>
      <Style TargetType="ScrollBar"><Setter Property="Width" Value="6"/><Setter Property="MinWidth" Value="6"/><Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ScrollBar"><Grid Width="6" Background="#111111"><Track x:Name="PART_Track" Orientation="Vertical" IsDirectionReversed="True"><Track.DecreaseRepeatButton><RepeatButton Command="ScrollBar.PageUpCommand" Opacity="0"/></Track.DecreaseRepeatButton><Track.IncreaseRepeatButton><RepeatButton Command="ScrollBar.PageDownCommand" Opacity="0"/></Track.IncreaseRepeatButton><Track.Thumb><Thumb><Thumb.Template><ControlTemplate TargetType="Thumb"><Border Background="#555555" Margin="1,0,1,0"/></ControlTemplate></Thumb.Template></Thumb></Track.Thumb></Track></Grid></ControlTemplate></Setter.Value></Setter></Style>
    </ResourceDictionary>
    """;
}

public sealed class UniformGridEx: System.Windows.Controls.Primitives.UniformGrid
{public UniformGridEx(int columns){Columns=columns;}}

public sealed class EspPreview:FrameworkElement
{
    readonly Runtime runtime;readonly DispatcherTimer timer;
    public EspPreview(Runtime r){runtime=r;timer=new DispatcherTimer{Interval=TimeSpan.FromMilliseconds(100)};timer.Tick+=(_,_)=>InvalidateVisual();Loaded+=(_,_)=>timer.Start();Unloaded+=(_,_)=>timer.Stop();}
    protected override void OnRender(DrawingContext dc)
    {
        dc.PushTransform(new ScaleTransform(1,ActualHeight/245));
        var c=runtime.Config;double cx=ActualWidth/2;var white=new SolidColorBrush(Color.FromArgb((byte)(c.EspOpacity*255),240,240,240));var pen=new Pen(white,c.LineWidth);
        dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(9,9,9)),null,new Rect(0,0,ActualWidth,ActualHeight));
        for(int i=0;i<ActualWidth;i+=24)dc.DrawLine(new Pen(new SolidColorBrush(Color.FromRgb(22,22,22)),1),new(i,0),new(i,ActualHeight));
        if(!c.Esp){dc.Pop();return;}
        if(c.Boxes){if(c.Corners){foreach(var(x,y,sx,sy)in new[]{(cx-43,30.0,1,1),(cx+43,30.0,-1,1),(cx-43,210.0,1,-1),(cx+43,210.0,-1,-1)}){dc.DrawLine(pen,new(x,y),new(x+sx*18,y));dc.DrawLine(pen,new(x,y),new(x,y+sy*18));}}else dc.DrawRectangle(null,pen,new Rect(cx-43,30,86,180));}
        if(c.Skeleton){foreach(var(a,b)in new[]{(new Point(cx,51),new Point(cx,118)),(new Point(cx,71),new Point(cx-24,95)),(new Point(cx-24,95),new Point(cx-31,131)),(new Point(cx,71),new Point(cx+24,95)),(new Point(cx+24,95),new Point(cx+31,131)),(new Point(cx,118),new Point(cx-18,161)),(new Point(cx-18,161),new Point(cx-22,203)),(new Point(cx,118),new Point(cx+18,161)),(new Point(cx+18,161),new Point(cx+22,203))})dc.DrawLine(pen,a,b);}
        if(c.HeadDot||c.Skeleton)dc.DrawEllipse(null,pen,new(cx,44),8,8);
        if(c.Health){dc.DrawRectangle(Brushes.DimGray,null,new Rect(cx-51,30,3,180));dc.DrawRectangle(Brushes.White,null,new Rect(cx-51,54,3,156));}
        void Text(string s,double y)=>dc.DrawText(new FormattedText(s,System.Globalization.CultureInfo.InvariantCulture,FlowDirection.LeftToRight,new Typeface("Segoe UI"),11,white,VisualTreeHelper.GetDpi(this).PixelsPerDip),new(cx-s.Length*2.8,y));
        if(c.Names)Text("opponent",7);if(c.Money||c.Distance)Text((c.Money?"$4200":"")+(c.Distance?"  /  24 m":""),220);
        dc.Pop();
    }
}

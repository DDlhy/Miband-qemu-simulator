using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Windows.Forms;

public class VelaBand3D : Control {
    struct V {
        public double x,y,z;
        public V(double a,double b,double c){x=a;y=b;z=c;}
    }
    class Face {
        public V[] points; public Color color; public bool screen; public double depth;
    }
    public double Yaw {get;set;}
    public double Pitch {get;set;}
    public double Roll {get;set;}
    public bool Vibrating {get;set;}
    public bool[] Leds = new bool[4];
    public double[] Pd = new double[2];
    public double GyroX {get;private set;}
    public double GyroY {get;private set;}
    public double GyroZ {get;private set;}
    public event EventHandler PoseChanged;
    public event EventHandler PdSelected;
    public int SelectedPd {get;private set;}
    public double AccelX {get{return 9.80665*Rotate(new V(1,0,0)).z;}}
    public double AccelY {get{return 9.80665*Rotate(new V(0,1,0)).z;}}
    public double AccelZ {get{return 9.80665*Rotate(new V(0,0,1)).z;}}
    public double MagX {get{V a=Rotate(new V(1,0,0));return 30*a.x+40*a.z;}}
    public double MagY {get{V a=Rotate(new V(0,1,0));return 30*a.x+40*a.z;}}
    public double MagZ {get{V a=Rotate(new V(0,0,1));return 30*a.x+40*a.z;}}
    Bitmap frame;
    bool dragging,moved;
    Point previous,origin;
    DateTime previousTime;
    double zoom=1;
    double phase;
    readonly Timer animation;
    readonly PointF[] pdPositions=new PointF[2];
    readonly bool[] pdVisible=new bool[2];
    public VelaBand3D() {
        SetStyle(ControlStyles.UserPaint|ControlStyles.AllPaintingInWmPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.ResizeRedraw,true);
        BackColor=Color.FromArgb(239,244,251);Size=new Size(630,540);Cursor=Cursors.Hand;
        animation=new Timer();animation.Interval=33;
        animation.Tick+=(s,e)=>{if(Vibrating){phase+=.9;Invalidate();}};animation.Start();
    }
    public void SetFrame(string path) {
        try {
            using(FileStream f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete))
            using(Image im=Image.FromStream(f)) {
                Bitmap next=new Bitmap(im);Bitmap old=frame;frame=next;if(old!=null)old.Dispose();
            }
            Invalidate();
        } catch(IOException){} catch(ArgumentException){}
    }
    V Rotate(V a) {
        double y=(Yaw%360)*Math.PI/180,p=(Pitch%360)*Math.PI/180,r=(Roll%360)*Math.PI/180;
        double xx=a.x*Math.Cos(y)+a.z*Math.Sin(y),zz=-a.x*Math.Sin(y)+a.z*Math.Cos(y);
        double yy=a.y*Math.Cos(p)-zz*Math.Sin(p);zz=a.y*Math.Sin(p)+zz*Math.Cos(p);
        return new V(xx*Math.Cos(r)-yy*Math.Sin(r),xx*Math.Sin(r)+yy*Math.Cos(r),zz);
    }
    PointF Project(V a) {
        double factor=Math.Min(Width/4.6,Height/6.5)*zoom*7/(7-a.z);
        double shake=Vibrating?Math.Sin(phase)*1.8:0;
        return new PointF((float)(Width/2+a.x*factor+shake),(float)(Height/2-a.y*factor));
    }
    void Add(List<Face> faces,Color color,bool screen,params V[] vertices) {
        Face f=new Face();f.points=new V[vertices.Length];f.color=color;f.screen=screen;
        for(int i=0;i<vertices.Length;i++){f.points[i]=Rotate(vertices[i]);f.depth+=f.points[i].z/vertices.Length;}
        faces.Add(f);
    }
    V[] Rounded(double w,double h,double radius,double z) {
        List<V> points=new List<V>();
        for(int i=0;i<4;i++) {
            double a=i*Math.PI/2;double cx=(i==0||i==3)?w/2-radius:-w/2+radius;
            double cy=i<2?h/2-radius:-h/2+radius;
            for(int n=0;n<=8;n++){double t=a+n*Math.PI/16;points.Add(new V(cx+radius*Math.Cos(t),cy+radius*Math.Sin(t),z));}
        }
        return points.ToArray();
    }
    void Circle(List<Face> faces,double x,double y,double z,double radius,Color color) {
        V[] ring=new V[20];for(int i=0;i<20;i++){double a=i*Math.PI/10;ring[i]=new V(x+radius*Math.Cos(a),y+radius*Math.Sin(a),z);}
        Add(faces,color,false,ring);
    }
    void DrawScreen(Graphics g,PointF[] p) {
        using(GraphicsPath path=new GraphicsPath()){path.AddPolygon(p);using(Brush b=new SolidBrush(Color.Black))g.FillPath(b,path);}
        if(frame==null)return;
        // Affine strips approximate the perspective plane while retaining the
        // real firmware framebuffer. Geometry and sensor locations remain 3D.
        for(int row=0;row<32;row++) {
            double y0=1.11-2.22*row/32,y1=1.11-2.22*(row+1)/32;
            PointF[] corners={Project(Rotate(new V(-.452,y0,.205))),Project(Rotate(new V(.452,y0,.205))),Project(Rotate(new V(-.452,y1,.205)))};
            GraphicsState saved=g.Save();using(GraphicsPath clip=new GraphicsPath()){clip.AddPolygon(p);g.SetClip(clip);}
            g.DrawImage(frame,corners,new RectangleF(0,frame.Height*row/32f,frame.Width,frame.Height/32f+1),GraphicsUnit.Pixel);g.Restore(saved);
        }
    }
    protected override void OnPaint(PaintEventArgs e) {
        base.OnPaint(e);Graphics g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;
        List<Face> faces=new List<Face>();
        Color metal=Color.FromArgb(72,79,89),back=Color.FromArgb(31,37,43),strap=Color.FromArgb(48,62,81);
        V[] front=Rounded(1.08,2.62,.44,.18),rear=Rounded(1.08,2.62,.44,-.18);
        for(int i=0;i<front.Length;i++) {
            int n=(i+1)%front.Length;Add(faces,metal,false,front[i],front[n],rear[n],rear[i]);
        }
        Add(faces,back,false,rear);Add(faces,Color.FromArgb(19,23,30),false,front);
        Add(faces,Color.Black,true,Rounded(.91,2.23,.35,.205));
        for(int sign=-1;sign<=1;sign+=2) {
            for(int segment=0;segment<12;segment++) {
                double a=1.24+segment*.12,b=a+.12;
                double za=-.14-Math.Pow(a-1.24,2)*.11,zb=-.14-Math.Pow(b-1.24,2)*.11;
                Add(faces,strap,false,new V(-.43,sign*a,za),new V(.43,sign*a,za),new V(.43,sign*b,zb),new V(-.43,sign*b,zb));
                Add(faces,Color.FromArgb(37,47,64),false,new V(-.43,sign*a,za-.09),new V(.43,sign*a,za-.09),new V(.43,sign*b,zb-.09),new V(-.43,sign*b,zb-.09));
                if(segment>5)Circle(faces,0,sign*(a+.06),za+.005,.035,Color.FromArgb(18,26,36));
            }
        }
        Circle(faces,0,0,-.205,.43,Color.FromArgb(10,15,22));
        Circle(faces,0,0,-.209,.35,Color.FromArgb(38,47,58));
        // Four visible packages, independent of the driver's eight logical LED
        // outputs. Cyan is an activity marker, not a claimed emission spectrum.
        for(int i=0;i<4;i++) {
            double x=(i%2==0?-.23:.23),y=(i<2?.20:-.20);
            Circle(faces,x,y,-.217,.073,Color.FromArgb(7,10,14));
            Circle(faces,x,y,-.22,.048,Leds[i]?Color.Aqua:Color.FromArgb(59,68,78));
        }
        for(int i=0;i<2;i++) {
            double x=0,y=(i==0?.12:-.12);
            double level=Math.Abs(Pd[i]);level=Double.IsNaN(level)?0:Math.Min(1,Math.Log10(level+1)/6);
            Circle(faces,x,y,-.222,.046,Color.FromArgb(25+(int)(70*level),59+(int)(130*level),85+(int)(150*level)));
            pdPositions[i]=Project(Rotate(new V(x,y,-.225)));pdVisible[i]=Rotate(new V(0,0,-1)).z>0;
        }
        faces.Sort((a,b)=>a.depth.CompareTo(b.depth));
        foreach(Face f in faces) {
            PointF[] points=new PointF[f.points.Length];for(int i=0;i<points.Length;i++)points[i]=Project(f.points[i]);
            if(f.screen&&Rotate(new V(0,0,1)).z>0)DrawScreen(g,points);
            else using(Brush brush=new SolidBrush(f.color))g.FillPolygon(brush,points);
        }
        DrawAxis(g,new V(1,0,0),Color.IndianRed,"X");DrawAxis(g,new V(0,1,0),Color.SeaGreen,"Y");DrawAxis(g,new V(0,0,1),Color.RoyalBlue,"Z");
        g.DrawString("左键拖动旋转 · 滚轮缩放 · 右键拖动旋转 Z 轴",Font,Brushes.DimGray,12,12);
        g.DrawString("示意外观 · 4 颗灯珠 / 2 个 PD · 发光颜色仅表示活动",Font,Brushes.DimGray,12,Height-28);
        if(Rotate(new V(0,0,-1)).z>0) {
            for(int i=0;i<2;i++)g.DrawString("PD"+(i+1),Font,Brushes.DarkCyan,pdPositions[i].X+12,pdPositions[i].Y-8);
        }
    }
    void DrawAxis(Graphics g,V direction,Color color,string name) {
        V a=Rotate(direction);float x=65+(float)a.x*35,y=Height-70-(float)a.y*35;
        using(Pen p=new Pen(color,2)){p.EndCap=LineCap.ArrowAnchor;g.DrawLine(p,65,Height-70,x,y);}using(Brush b=new SolidBrush(color))g.DrawString(name,Font,b,x,y);
    }
    protected override void OnMouseDown(MouseEventArgs e) {
        base.OnMouseDown(e);dragging=true;moved=false;Capture=true;previous=origin=e.Location;previousTime=DateTime.UtcNow;
    }
    protected override void OnMouseMove(MouseEventArgs e) {
        base.OnMouseMove(e);if(!dragging)return;
        double seconds=Math.Max(.008,(DateTime.UtcNow-previousTime).TotalSeconds);
        double dx=(e.X-previous.X)*.6,dy=(e.Y-previous.Y)*.6;
        double yaw=(Yaw+dx*.5)*Math.PI/180,pitch=(Pitch+dy*.5)*Math.PI/180;
        double yd=0,pd=0,rd=0;
        if((e.Button&MouseButtons.Right)!=0){Roll+=dx;rd=dx/seconds;}
        else{Yaw+=dx;Pitch+=dy;yd=dx/seconds;pd=dy/seconds;}
        // Rotate() is Rz(Roll) Rx(Pitch) Ry(Yaw). Resolve its angular
        // velocity in the sensor frame, matching the gravity/field vectors.
        GyroX=pd*Math.Cos(yaw)-rd*Math.Sin(yaw)*Math.Cos(pitch);
        GyroY=yd+rd*Math.Sin(pitch);
        GyroZ=pd*Math.Sin(yaw)+rd*Math.Cos(yaw)*Math.Cos(pitch);
        previous=e.Location;previousTime=DateTime.UtcNow;moved|=Math.Abs(e.X-origin.X)+Math.Abs(e.Y-origin.Y)>5;
        Invalidate();if(PoseChanged!=null)PoseChanged(this,EventArgs.Empty);
    }
    protected override void OnMouseUp(MouseEventArgs e) {
        base.OnMouseUp(e);dragging=false;Capture=false;GyroX=GyroY=GyroZ=0;
        if(moved){if(PoseChanged!=null)PoseChanged(this,EventArgs.Empty);}
        else for(int i=0;i<2;i++)if(pdVisible[i]&&Math.Abs(e.X-pdPositions[i].X)<12&&Math.Abs(e.Y-pdPositions[i].Y)<12){SelectedPd=i;if(PdSelected!=null)PdSelected(this,EventArgs.Empty);break;}
    }
    protected override void OnMouseCaptureChanged(EventArgs e) {
        base.OnMouseCaptureChanged(e);
        if(dragging&&!Capture){dragging=false;GyroX=GyroY=GyroZ=0;if(PoseChanged!=null)PoseChanged(this,EventArgs.Empty);}
    }
    protected override void OnMouseWheel(MouseEventArgs e){base.OnMouseWheel(e);zoom=Math.Max(.25,Math.Min(4,zoom*Math.Pow(1.1,e.Delta/120.0)));Invalidate();}
    protected override void Dispose(bool disposing){if(disposing){animation.Dispose();if(frame!=null)frame.Dispose();}base.Dispose(disposing);}
}
